/*
                                                                                     ███████
 ████████████  ████████████   ████████████       █  █████████████  █           █  ███       ███  ████████████
█              █ █           █            █    █ █  █              █        ███      ███████    █            █
 ████████████  █   █         █████████████   █   █   █             █   █████      ███       ███ █████████████
             █ █     █       █            █      █    █            ████      █                  █            █
 ████████████  █       █     █            █      █      █████████  █          █   ███       ███ █            █
                                                                                     ███████
 inverseKinematicsPlugin.cpp

 Inverse Kinematics Plugin
 Created 04/03/2023

 Copyright (C) 2023 Valeriy Novytskyy
 This software is licensed under GNU GPLv3
*/

/*----------------------------------------------------------*\
| Includes
\*----------------------------------------------------------*/

#include <cmath>
#include <Eigen/Geometry>
#include <pluginlib/class_list_macros.h>
#include <eigen_conversions/eigen_msg.h>
#include <tf2_eigen/tf2_eigen.h>
#include "inverseKinematicsSolver.h"
#include "inverseKinematicsPlugin.h"

/*----------------------------------------------------------*\
| Namespace
\*----------------------------------------------------------*/

using namespace std;
using namespace kinematics;
using namespace moveit::core;
using namespace robot_state;
using namespace moveit_msgs;
using namespace visualization_msgs;
using namespace Eigen;
using namespace str1ker;

/*----------------------------------------------------------*\
| Variables
\*----------------------------------------------------------*/

const char PLUGIN_NAME[] = "str1ker::ik";

/*----------------------------------------------------------*\
| IKPlugin implementation
\*----------------------------------------------------------*/

//
// Constructor
//

IKPlugin::IKPlugin() :
    m_pPlanningGroup(NULL),
    m_node("~"),
    m_pBaseJoint(NULL),
    m_pShoulderJoint(NULL),
    m_pElbowJoint(NULL),
    m_pWristJoint(NULL),
    m_positionOnly(true),
    m_debug(false)
{
}

//
// Public methods
//

bool IKPlugin::initialize(
    const RobotModel &robot_model,
    const string &group_name,
    const string &base_frame,
    const vector<string> &tip_frames,
    double search_discretization)
{
    ROS_INFO_NAMED(PLUGIN_NAME, "Str1ker IK Plugin Initializing");

    // Retrieve planning group
    m_pPlanningGroup = robot_model.getJointModelGroup(group_name);

    if (!m_pPlanningGroup)
    {
        ROS_ERROR_NAMED(PLUGIN_NAME, "Failed to retrieve joint model group");
        return false;
    }

    // Load settings
    if (!m_node.getParam(
        string("/robot_description_kinematics/") + group_name + "/positionOnly",
        m_positionOnly))
    {
        ROS_INFO_NAMED(PLUGIN_NAME, "Defaulting to position-only IK");
    }
    else
    {
        ROS_INFO_NAMED(PLUGIN_NAME, "Using position-only IK: %s", m_positionOnly ? "true" : "false");
    }

    if (m_node.getParam(string("/robot_description_kinematics/") + group_name + "/debug",
        m_debug))
    {
        ROS_INFO_NAMED(PLUGIN_NAME, m_debug ? "Debugging enabled" : "Debugging disabled");
    }

    // Validate chains
    auto chains = m_pPlanningGroup->getConfig().chains_;

    if (chains.size() != 1)
    {
        ROS_ERROR_NAMED(
            PLUGIN_NAME,
            "Only one chain supported in planning group, found %ld",
            chains.size());

        return false;
    }
    
    ROS_INFO_NAMED(
        PLUGIN_NAME,
        "Chain: %s -> %s",
        chains[0].first.c_str(),
        chains[0].second.c_str());

    // Validate tips
    if (tip_frames.size() != 1)
    {
        ROS_ERROR_NAMED(
            PLUGIN_NAME,
            "Only one tip frame supported, found %ld",
            tip_frames.size());

        return false;
    }

    // Validate joints
    auto joints = m_pPlanningGroup->getJointModels();
    auto jointNames = m_pPlanningGroup->getJointModelNames();

    for (size_t jointIndex = 0;
         jointIndex < joints.size();
         jointIndex++)
    {
        const JointModel *pJoint = joints[jointIndex];

        if (pJoint->getType() == JointModel::REVOLUTE)
        {
            auto limits = pJoint->getVariableBoundsMsg().front();

            ROS_INFO_NAMED(
                PLUGIN_NAME,
                "Joint %s: %s %sfrom %g to %g",
                jointNames[jointIndex].c_str(),
                "revolute",
                pJoint->getMimic() != NULL ? "mimic " : "",
                limits.min_position,
                limits.max_position);

            m_joints.push_back(pJoint);
        }
        else if (pJoint->getType() == JointModel::PRISMATIC)
        {
            auto limits = pJoint->getVariableBoundsMsg().front();

            ROS_INFO_NAMED(
                PLUGIN_NAME,
                "Joint %s: %s %sfrom %g to %g",
                jointNames[jointIndex].c_str(),
                "prismatic",
                pJoint->getMimic() != NULL ? "mimic " : "",
                limits.min_position,
                limits.max_position);

            m_joints.push_back(pJoint);
        }
    }

    // Validate links
    auto linkNames = m_pPlanningGroup->getLinkModelNames();

    for (size_t linkIndex = 0;
         linkIndex < linkNames.size();
         linkIndex++)
    {
        ROS_INFO_NAMED(PLUGIN_NAME, "Link %s", linkNames[linkIndex].c_str());
    }

    // Load configuration
    vector<string> chainTips;
    auto chainTip = chains.front().second;
    chainTips.push_back(chainTip);

    ROS_INFO_NAMED(
        PLUGIN_NAME,
        "Initializing with base %s and tip %s",
        base_frame.c_str(),
        chainTip.c_str());

    KinematicsBase::storeValues(
        robot_model,
        group_name,
        base_frame,
        chainTips,
        search_discretization);

    m_pBaseJoint = getJoint(JointModel::REVOLUTE);
    m_pShoulderJoint = getJoint(JointModel::REVOLUTE, m_pBaseJoint);
    m_pElbowJoint = getJoint(JointModel::REVOLUTE, m_pShoulderJoint);
    m_pWristJoint = getJoint(JointModel::REVOLUTE, m_pElbowJoint);

    // Initialize state
    m_pState.reset(new robot_state::RobotState(robot_model_));
    m_pState->setToDefaultValues();

    // Advertise marker publisher
    if (m_debug)
    {
        m_markerPub = m_node.advertise<visualization_msgs::Marker>(
            "visualization_marker",
            10);
    }

    return true;
}

bool IKPlugin::supportsGroup(
    const JointModelGroup *jmg, string *error_text_out) const
{
    if (!jmg->isSingleDOFJoints())
    {
        *error_text_out = "IK solver supports only single DOF joints";
        return false;
    }

    return true;
}

const vector<string> &IKPlugin::getJointNames() const
{
    return m_pPlanningGroup->getJointModelNames();
}

const vector<string> &IKPlugin::getLinkNames() const
{
    return m_pPlanningGroup->getLinkModelNames();
}

bool IKPlugin::getPositionFK(
    const vector<string> &link_names,
    const vector<double> &joint_angles,
    vector<geometry_msgs::Pose> &poses) const
{
    MatrixXd angles((int)COUNT, 1);

    for (int n = 0; n < link_names.size(); n++)
    {
        if (link_names[n] == "base")
        {
            angles(BASE, 0) = joint_angles[n];
        }
        else if (link_names[n] == "shoulder")
        {
            angles(SHOULDER, 0) = joint_angles[n];
        }
        else if (link_names[n] == "elbow")
        {
            angles(ELBOW, 0) = joint_angles[n];
        }
        else if (link_names[n] == "wrist")
        {
            angles(WRIST, 0) = joint_angles[n];
        }
    }

    Isometry3d pose = forwardKinematics(angles);

    geometry_msgs::Pose poseMsg;
    tf::poseEigenToMsg(pose, poseMsg);
    poses.push_back(poseMsg);

    return true;
}

bool IKPlugin::getPositionIK(
    const geometry_msgs::Pose &ik_pose,
    const vector<double> &ik_seed_state,
    vector<double> &solution,
    MoveItErrorCodes &error_code,
    const KinematicsQueryOptions &options) const
{
    return searchPositionIK(
        ik_pose,
        ik_seed_state,
        DEFAULT_TIMEOUT,
        solution,
        error_code,
        options);
}

bool IKPlugin::searchPositionIK(
    const geometry_msgs::Pose &ik_pose,
    const vector<double> &ik_seed_state,
    double timeout,
    vector<double> &solution,
    MoveItErrorCodes &error_code,
    const KinematicsQueryOptions &options) const
{
    const IKCallbackFn solution_callback = NULL;
    vector<double> consistency_limits;
    vector<geometry_msgs::Pose> poses;
    poses.push_back(ik_pose);

    return searchPositionIK(
        poses,
        ik_seed_state,
        timeout,
        consistency_limits,
        solution,
        solution_callback,
        error_code,
        options);
}

bool IKPlugin::searchPositionIK(
    const geometry_msgs::Pose &ik_pose,
    const vector<double> &ik_seed_state,
    double timeout,
    const vector<double> &consistency_limits,
    vector<double> &solution,
    MoveItErrorCodes &error_code,
    const KinematicsQueryOptions &options) const
{
    const IKCallbackFn solution_callback = NULL;

    return searchPositionIK(
        ik_pose,
        ik_seed_state,
        timeout,
        consistency_limits,
        solution,
        solution_callback,
        error_code,
        options);
}

bool IKPlugin::searchPositionIK(
    const geometry_msgs::Pose &ik_pose,
    const vector<double> &ik_seed_state,
    double timeout,
    vector<double> &solution,
    const IKCallbackFn &solution_callback,
    MoveItErrorCodes &error_code,
    const KinematicsQueryOptions &options) const
{
    vector<double> consistency_limits;

    return searchPositionIK(
        ik_pose,
        ik_seed_state,
        timeout,
        consistency_limits,
        solution,
        solution_callback,
        error_code,
        options);
}

bool IKPlugin::searchPositionIK(
    const geometry_msgs::Pose &ik_pose,
    const vector<double> &ik_seed_state,
    double timeout,
    const vector<double> &consistency_limits,
    vector<double> &solution,
    const IKCallbackFn &solution_callback,
    MoveItErrorCodes &error_code,
    const KinematicsQueryOptions &options) const
{
    vector<geometry_msgs::Pose> poses;
    poses.push_back(ik_pose);

    return searchPositionIK(
        poses,
        ik_seed_state,
        timeout,
        consistency_limits,
        solution,
        solution_callback,
        error_code,
        options);
}

bool IKPlugin::searchPositionIK(
    const vector<geometry_msgs::Pose> &ik_poses,
    const vector<double> &ik_seed_state,
    double timeout,
    const vector<double> &consistency_limits,
    vector<double> &solution,
    const IKCallbackFn &solution_callback,
    MoveItErrorCodes &error_code,
    const KinematicsQueryOptions &options,
    const robot_state::RobotState *context_state) const
{
    // Initialize solution
    solution.resize(m_joints.size());

    // Validate request
    if (!validateSeedState(ik_seed_state) || !validateTarget(ik_poses))
    {
        error_code.val = error_code.NO_IK_SOLUTION;
        return false;
    }

    // Solve inverse kinematics
    Vector3d origin = getOrigin();
    MatrixXd angles;

    if (m_positionOnly)
    {
        Vector3d goal = getGoalPosition(ik_poses, origin);
        angles = inverseKinematics(goal);
    }
    else
    {
        Matrix4d goal = getGoal(ik_poses, origin);
        angles = inverseKinematics(goal);
    }

    // Visualize solution
    if (m_debug) visualizeSolution(origin, angles);

    // Return solution
    setJointState(m_pBaseJoint, angles(BASE, 0), solution);
    setJointState(m_pShoulderJoint, angles(SHOULDER, 0), solution);
    setJointState(m_pElbowJoint, angles(ELBOW, 0), solution);
    setJointState(m_pWristJoint, angles(WRIST, 0), solution);

    error_code.val = error_code.SUCCESS;

    if (!solution_callback.empty())
    {
        solution_callback(ik_poses.front(), solution, error_code);
    }

    return true;
}

//
// Private methods
//

const JointModel* IKPlugin::getJoint(
    JointModel::JointType type, const JointModel* parent) const
{
    for (auto joint : m_joints)
    {
        if (parent)
        {
            if (joint->getType() == type && joint->getParentLinkModel() == parent->getChildLinkModel())
                return joint;
        }
        else
        {
            if (joint->getType() == type)
                return joint;
        }
    }

    return nullptr;
}

bool IKPlugin::validateTarget(const vector<geometry_msgs::Pose>& ik_poses) const
{
    if (ik_poses.size() != 1 || tip_frames_.size() != ik_poses.size())
    {
        ROS_ERROR_NAMED(
            PLUGIN_NAME,
            "Found %ld tips and %ld poses (expected one pose and one tip)",
            tip_frames_.size(),
            ik_poses.size());

        return false;
    }

    return true;
}

Matrix4d IKPlugin::getGoal(const std::vector<geometry_msgs::Pose>& ik_poses, const Eigen::Vector3d& origin) const
{
    Isometry3d goal;
    auto goalPose = ik_poses.back();
    tf2::fromMsg(goalPose, goal);

    Matrix4d goalMatrix = goal.matrix();
    goalMatrix(0, 3) = goalMatrix(0, 3) - origin.x();
    goalMatrix(1, 3) = goalMatrix(1, 3) - origin.y();
    goalMatrix(2, 3) = goalMatrix(2, 3) - origin.z();

    ROS_DEBUG_NAMED(
        PLUGIN_NAME,
        "IK goal %s [\n\t%g, %g, %g, %g;\n\t%g, %g, %g, %g;\n\t%g, %g, %g, %g;\n\t%g, %g, %g, %g\n]",
        tip_frames_.front().c_str(),
        goalMatrix(0, 0),
        goalMatrix(0, 1),
        goalMatrix(0, 2),
        goalMatrix(0, 3),
        goalMatrix(1, 0),
        goalMatrix(1, 1),
        goalMatrix(1, 2),
        goalMatrix(1, 3),
        goalMatrix(2, 0),
        goalMatrix(2, 1),
        goalMatrix(2, 2),
        goalMatrix(2, 3),
        goalMatrix(3, 0),
        goalMatrix(3, 1),
        goalMatrix(3, 2),
        goalMatrix(3, 3)
    );

    return goalMatrix;
}

Vector3d IKPlugin::getGoalPosition(const vector<geometry_msgs::Pose>& ik_poses, const Eigen::Vector3d& origin) const
{
    Isometry3d goal;
    auto goalPose = ik_poses.back();
    tf2::fromMsg(goalPose, goal);

    Vector3d pos = goal.translation();
    pos.x() = pos.x() - origin.x();
    pos.y() = pos.y() - origin.y();
    pos.z() = pos.z() - origin.z();

    ROS_DEBUG_NAMED(
        PLUGIN_NAME,
        "IK goal %s: %g, %g, %g",
        tip_frames_.front().c_str(),
        pos.x(),
        pos.y(),
        pos.z()
    );

    return pos;
}

Vector3d IKPlugin::getOrigin() const
{
    return m_pState->getGlobalLinkTransform(m_pBaseJoint->getChildLinkModel())
        .translation();
}

bool IKPlugin::validateSeedState(const vector<double>& ik_seed_state) const
{
    if (ik_seed_state.size() != m_joints.size())
    {
        ROS_ERROR_NAMED(
            PLUGIN_NAME,
            "Expected seed state for %ld supported joints, received state for %ld",
            m_joints.size(),
            ik_seed_state.size());

        return false;
    }

    ROS_DEBUG_NAMED(
        PLUGIN_NAME,
        "Received seed state for %ld joints",
        ik_seed_state.size()
    );

    for (size_t jointIndex = 0;
        jointIndex < ik_seed_state.size();
        jointIndex++)
    {
        ROS_DEBUG_NAMED(
            PLUGIN_NAME,
            "\t%s (%s): %g",
            m_joints[jointIndex]->getName().c_str(),
            m_joints[jointIndex]->getMimic() ? "mimic" : "active",
            ik_seed_state[jointIndex]
        );
    }

    return true;
}

void IKPlugin::publishArrowMarker(int id, vector<Vector3d> points, Vector3d color) const
{
    Marker marker;
    marker.id = id;
    marker.type = Marker::ARROW;
    marker.header.frame_id = "world";
    marker.scale.x = 0.01;
    marker.scale.y = 0.05;
    marker.color.r = color.x();
    marker.color.g = color.y();
    marker.color.b = color.z();
    marker.color.a = 1.0;

    for (const Vector3d& point : points)
    {
        geometry_msgs::Point msgPoint;
        msgPoint.x = point.x();
        msgPoint.y = point.y();
        msgPoint.z = point.z();
        marker.points.push_back(msgPoint);
    }

    m_markerPub.publish(marker);
}

void IKPlugin::visualizeSolution(const Vector3d& origin, const MatrixXd& angles) const
{
    Vector3d shoulderPose = origin + forwardKinematics(angles.block(0, 0, 2, 1))
        .matrix()
        .block(0, 3, 3, 1);
    Vector3d elbowPose = origin + forwardKinematics(angles.block(0, 0, 3, 1))
        .matrix()
        .block(0, 3, 3, 1);
    Vector3d wristPose = origin + forwardKinematics(angles.block(0, 0, 4, 1))
        .matrix()
        .block(0, 3, 3, 1);

    publishArrowMarker(
        SHOULDER, { shoulderPose, elbowPose }, Vector3d(1.0, 0.0, 1.0));
    publishArrowMarker(
        ELBOW, { elbowPose, wristPose }, Vector3d(0.0, 1.0, 1.0));
}

//
// Static methods
//

Isometry3d IKPlugin::setJointState(
    const JointModel* pJoint,
    double angle,
    std::vector<double>& states) const
{
    const Vector3d& axis = getJointAxis(pJoint);
    const JointLimits& limits = pJoint->getVariableBoundsMsg().front();
    double jointState = isnan(angle)
        ? limits.max_position
        : clamp(angle, limits.min_position, limits.max_position);

    size_t index = find(m_joints.begin(), m_joints.end(), pJoint) - m_joints.begin();
    states[index] = jointState;

    ROS_DEBUG_NAMED(PLUGIN_NAME, "IK solution %s: %g [%g] min %g max %g",
        pJoint->getName().c_str(), angle, jointState, limits.min_position,
        limits.max_position);

    if (pJoint->getMimic())
    {
        // Update the master joint
        const JointModel* pMasterJoint = pJoint->getMimic();
        const JointLimits& masterLimits = pMasterJoint->getVariableBoundsMsg().front();

        double masterState = clamp(
            (jointState - pJoint->getMimicOffset()) / pJoint->getMimicFactor(),
            masterLimits.min_position,
            masterLimits.max_position);

        size_t masterIndex =
            find(m_joints.begin(), m_joints.end(), pMasterJoint)
            - m_joints.begin();

        states[masterIndex] = masterState;

        m_pState->setJointPositions(pMasterJoint, &masterState);
        m_pState->enforceBounds(pMasterJoint);

        for (auto pMimicJoint : pMasterJoint->getMimicRequests())
        {
            if (pMimicJoint == pJoint) continue;

            auto mimicPos = find(m_joints.begin(), m_joints.end(), pMimicJoint);
            if (mimicPos == m_joints.end()) continue;

            size_t mimicIndex = mimicPos - m_joints.begin();
            const JointLimits& mimicLimits =
                pMimicJoint->getVariableBoundsMsg().front();

            double mimicState = clamp(
                masterState
                * pMimicJoint->getMimicFactor()
                + pMimicJoint->getMimicOffset(),
                mimicLimits.min_position,
                mimicLimits.max_position);

            states[mimicIndex] = mimicState;

            m_pState->setJointPositions(pMimicJoint, &mimicState);
            m_pState->enforceBounds(pMimicJoint);
        }
    }
    else
    {
        m_pState->setJointPositions(pJoint, &jointState);
        m_pState->enforceBounds(pJoint);
    }

    return m_pState->getJointTransform(pJoint);
}

const Vector3d& IKPlugin::getJointAxis(const JointModel* pJoint)
{
    if (pJoint->getType() == JointModel::REVOLUTE)
    {
        return dynamic_cast<const RevoluteJointModel*>(pJoint)->getAxis();
    }
    else
    {
        return dynamic_cast<const PrismaticJointModel*>(pJoint)->getAxis();
    }
}

PLUGINLIB_EXPORT_CLASS(str1ker::IKPlugin, kinematics::KinematicsBase);
