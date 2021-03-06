/**********************************************************************************************************************
This file is part of the Control Toolbox (https://github.com/ethz-adrl/control-toolbox), copyright by ETH Zurich.
Licensed under BSD-2 license (see LICENSE file in main directory)
**********************************************************************************************************************/

//#define MATLAB
//#define MATLAB_FULL_LOG
//#define DEBUG_PRINT_MP

#include <ct/rbd/rbd.h>
#include <ct/ros/ros.h>

#include <ct/models/HyA/HyA.h>
#include <ct/models/HyA/HyAInverseKinematics.h>

#include "CollisionCostTermCG.hpp"

using namespace ct::rbd;

const size_t njoints = ct::rbd::HyA::Kinematics::NJOINTS;
const size_t state_dim = 2*njoints;
const size_t control_dim = njoints;

using HyADynamics = ct::rbd::HyA::Dynamics;
using HyASystem = ct::rbd::FixBaseAccSystem<HyADynamics>;
using LinearSystem = ct::core::LinearSystem<state_dim, control_dim>;
using SystemLinearizer = ct::core::SystemLinearizer<state_dim, control_dim>;
using StateVector = ct::core::StateVector<state_dim>;
using NLOptConSolver = ct::optcon::NLOptConSolver<state_dim, control_dim>;

StateVector x0;  // init state


//! compute linearly interpolated or steady initial guess
ct::optcon::NLOptConSolver<state_dim, control_dim>::Policy_t computeInitialGuess(int initType,
    const size_t nSteps,
    const StateVector& x_0,
    const StateVector& x_f,
    double dt)
{
    // provide initial guess
    ct::core::StateVectorArray<state_dim> x0(nSteps + 1, x_0);
    ct::core::ControlVectorArray<control_dim> u0(nSteps, ct::core::ControlVector<control_dim>::Zero());
    ct::core::FeedbackArray<state_dim, control_dim> u0_fb(
        nSteps, ct::core::FeedbackMatrix<state_dim, control_dim>::Zero());

    switch (initType)
    {
        case 0:  // steady state
        {
            // do nothing, that is the default initialization
            break;
        }
        case 1:  // linear interpolation
        {
            for (size_t i = 0; i < nSteps + 1; i++)
                x0[i] = x_0 + (x_f - x_0) * ((double)i / (double)(nSteps));

            break;
        }
        default:
        {
            throw std::runtime_error("illegal init type");
            break;
        }
    }

    return ct::optcon::NLOptConSolver<state_dim, control_dim>::Policy_t(x0, u0, u0_fb, dt);
}


//! loop through all cost function terms in NLOC and update their desired final states
void updateCostFunctionFinalStates(const StateVector& x_f,
    ct::optcon::NLOptConSolver<state_dim, control_dim>& nloc)
{
    // get vector of pointers to the cost functions
    std::vector<std::shared_ptr<ct::optcon::CostFunctionQuadratic<state_dim, control_dim>>>& costInst =
        nloc.getCostFunctionInstances();

    // update the joint-level cost functions with sampled reference state
    for (size_t i = 0; i < costInst.size(); i++)
    {
        costInst[i]->updateReferenceState(x_f);
        costInst[i]->updateFinalState(x_f);
    }
}


int main(int argc, char* argv[])
{
    ros::init(argc, argv, "hya_kin_taskspace");
    ros::NodeHandle nh("~");

    ROS_INFO("Set up visualizers");
    std::string frameId = "/world";
    ct::ros::RBDStatePublisher statePublisher(ct::models::HyA::urdfJointNames(), "/hya/hya_base_link", frameId);
    statePublisher.advertise(nh, "/joint_states", 10);

    std::shared_ptr<ct::ros::PoseVisualizer> targetPoseVisualizer(new ct::ros::PoseVisualizer(frameId, "robot_1"));
    std::shared_ptr<ct::ros::PoseVisualizer> currentPoseVisualizer(new ct::ros::PoseVisualizer(frameId, "robot_2"));

    ct::ros::VisNode<geometry_msgs::PoseStamped> visNode_poseDes(nh, std::string("ee_ref_pose_visualizer"));
    ct::ros::VisNode<geometry_msgs::PoseStamped> visNode_poseCurr(nh, std::string("ee_current_pose_visualizer"));

    visNode_poseDes.addVisualizer(targetPoseVisualizer);
    visNode_poseCurr.addVisualizer(currentPoseVisualizer);

    ROS_INFO("Loading NLOC config files");

    std::string workingDirectory;
    if (!nh.getParam("workingDirectory", workingDirectory))
        std::cout << "Working directory parameter 'workingDirectory' not set" << std::endl;

    std::cout << "Working directory is: " << workingDirectory << std::endl;
    std::string configFile = workingDirectory + "/solver.info";
    std::string costFunctionFile = workingDirectory + "/cost.info";

    ROS_INFO("Setting up system");
    std::shared_ptr<HyASystem> system(new HyASystem);
    std::shared_ptr<LinearSystem> linSystem(new SystemLinearizer(system));

    ROS_INFO("Initializing NLOC");

    // load x0
    ct::core::loadMatrix(costFunctionFile, "x_0", x0);

    // load init feedback
    ct::core::FeedbackMatrix<state_dim, control_dim> fbD;
    ct::core::loadMatrix(costFunctionFile, "K_init", fbD);

    // NLOC settings
    ct::optcon::NLOptConSettings nloc_settings;
    nloc_settings.load(configFile, true, "nloc");

    // get time horizon
    double tf = 3.0;
    ct::core::loadScalar(configFile, "timeHorizon", tf);
    size_t nSteps = nloc_settings.computeK(tf);

    // Setup Costfunction
    std::shared_ptr<ct::optcon::CostFunctionAnalytical<state_dim, control_dim>> costFun(
        new ct::optcon::CostFunctionAnalytical<state_dim, control_dim>());

    ROS_INFO("Setting up task-space cost term");
    using HyAKinematicsAD_t = HyA::tpl::Kinematics<ct::core::ADCGScalar>;
    using HyAKinematics_t = HyA::tpl::Kinematics<double>;


    // obstacle cost term
    using CollisionCostTermCG = ct::rbd::CollisionCostTermCG<HyAKinematicsAD_t, false, state_dim, control_dim>;

    std::shared_ptr<CollisionCostTermCG> termObstacle_final(new CollisionCostTermCG(costFunctionFile, "termObstacle", true));
    std::shared_ptr<CollisionCostTermCG> termObstacle_intermediate(new CollisionCostTermCG(costFunctionFile, "termObstacle", true));

    size_t task_space_final_term_id = costFun->addFinalTerm(termObstacle_final, true);
    size_t task_space_intermediate_term_id = costFun->addIntermediateTerm(termObstacle_intermediate, true);

    ct::rbd::RigidBodyPose ee_pose_des;
    ee_pose_des.position() = ct::rbd::RigidBodyPose::Position3Tpl (termObstacle_intermediate->getReferencePosition());



    ROS_INFO("Setting up joint-space cost terms");
    using TermQuadratic = ct::optcon::TermQuadratic<state_dim, control_dim>;
    std::shared_ptr<TermQuadratic> termQuadInterm(new TermQuadratic(costFunctionFile, "term_quad_intermediate"));
    std::shared_ptr<TermQuadratic> termQuadFinal(new TermQuadratic(costFunctionFile, "term_quad_final"));

    size_t intTermID = costFun->addIntermediateTerm(termQuadInterm);
    size_t term_quad_final_id = costFun->addFinalTerm(termQuadFinal);
    costFun->initialize();

    StateVector xf;  // temporary final state
    xf = termQuadFinal->getReferenceState();

    /* STEP 1-D: set up the general constraints */
    // constraint terms
    ROS_INFO("Setting up joint-space constraints");

   // create constraint container
    std::shared_ptr<ct::optcon::ConstraintContainerAnalytical<state_dim, control_dim>> inputBoxConstraints(
        new ct::optcon::ConstraintContainerAnalytical<state_dim, control_dim>());

    std::shared_ptr<ct::optcon::ConstraintContainerAnalytical<state_dim, control_dim>> stateBoxConstraints(
        new ct::optcon::ConstraintContainerAnalytical<state_dim, control_dim>());

    // input constraint bounds
    ct::core::ControlVector<control_dim> u_lb = -100 * ct::core::ControlVector<control_dim>::Ones();
    ct::core::ControlVector<control_dim> u_ub = 100 * ct::core::ControlVector<control_dim>::Ones();
    // input constraint terms
    std::shared_ptr<ct::optcon::ControlInputConstraint<state_dim, control_dim>> controlConstraint(
        new ct::optcon::ControlInputConstraint<state_dim, control_dim>(u_lb, u_ub));
    controlConstraint->setName("ControlInputConstraint");
        // add and initialize constraint terms
    inputBoxConstraints->addIntermediateConstraint(controlConstraint, true);
    inputBoxConstraints->initialize();
    
    // state constraint bounds
    ct::core::StateVector<state_dim> x_lb, x_ub;
    x_lb.head<njoints>() = -3.14 * Eigen::Matrix<double, njoints, 1>::Ones(); // lower bound on position
    x_ub.head<njoints>() =  3.14 * Eigen::Matrix<double, njoints, 1>::Ones(); // upper bound on position
    x_lb.tail<njoints>() = -100 * Eigen::Matrix<double, njoints, 1>::Ones(); // lower bound on velocity
    x_ub.tail<njoints>() =  100 * Eigen::Matrix<double, njoints, 1>::Ones(); // upper bound on velocity
    // state constraint terms
    std::shared_ptr<ct::optcon::StateConstraint<state_dim, control_dim>> stateConstraint(
        new ct::optcon::StateConstraint<state_dim, control_dim>(x_lb, x_ub));
    stateConstraint->setName("StateConstraint");
    // add and initialize constraint terms    
    stateBoxConstraints->addIntermediateConstraint(stateConstraint, true);
    stateBoxConstraints->addTerminalConstraint(stateConstraint,true);
    stateBoxConstraints->initialize();

    ROS_INFO("Creating optcon problem now");
    ct::optcon::ContinuousOptConProblem<state_dim, control_dim> optConProblem(tf, x0, system, costFun, linSystem);
    // add the box constraints to the optimal control problem
    optConProblem.setInputBoxConstraints(inputBoxConstraints);
    optConProblem.setStateBoxConstraints(stateBoxConstraints);

    ROS_INFO("Creating solver now");
    NLOptConSolver nloc(optConProblem, nloc_settings);
    nloc.configure(nloc_settings);


    int initType = 0;
    ct::core::loadScalar(configFile, "initType", initType);
    NLOptConSolver::Policy_t initController = computeInitialGuess(initType, nSteps, x0, xf, nloc_settings.dt);
    nloc.setInitialGuess(initController);

    updateCostFunctionFinalStates(xf, nloc);


    ROS_INFO("Solving problem ...");

    nloc.solve();
    ct::core::StateVectorArray<state_dim> x_solution = nloc.getSolution().x_ref();


    do
    {
      std::cout << '\n' << "Press a key to continue...";
    } while (std::cin.get() != '\n');

    while (ros::ok())
    {
        ROS_INFO("Visualizing ...");

        ros::Rate publishRate(1. / nloc.getSettings().dt);

        for (size_t i = 0; i < x_solution.size(); i++)
        {
            targetPoseVisualizer->setPose(ee_pose_des);
            visNode_poseDes.visualize();

            ct::rbd::HyA::Kinematics kinematics;
            size_t eeInd = 0;
            ct::rbd::RigidBodyPose eePoseCurr =
                kinematics.getEEPoseInBase(eeInd, x_solution[i].template cast<double>().head<6>());
            currentPoseVisualizer->setPose(eePoseCurr);
            visNode_poseCurr.visualize();

            RBDState<njoints> state;
            state.setZero();
            state.jointPositions() = x_solution[i].template cast<double>().head<6>();
//            state.jointVelocities() = x_solution[i].template cast<double>().tail<6>();

            statePublisher.publishState(state);
            publishRate.sleep();
        }

        ros::Duration(1.0).sleep();
    }
}
