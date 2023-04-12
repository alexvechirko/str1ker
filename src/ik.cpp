/*
                                                                                     ███████
 ████████████  ████████████   ████████████       █  █████████████  █           █  ███       ███  ████████████
█              █ █           █            █    █ █  █              █        ███      ███████    █            █
 ████████████  █   █         █████████████   █   █   █             █   █████      ███       ███ █████████████
             █ █     █       █            █      █    █            ████      █                  █            █
 ████████████  █       █     █            █      █      █████████  █          █   ███       ███ █            █
                                                                                     ███████
 ik.cpp

 Inverse Kinematics Plugin
 Created 04/03/2023

 Copyright (C) 2023 Valeriy Novytskyy
 This software is licensed under GNU GPLv3
*/

/*----------------------------------------------------------*\
| Includes
\*----------------------------------------------------------*/

#include <Eigen/Geometry>
#include <tf2_eigen/tf2_eigen.h>
#include "include/ik.h"

/*----------------------------------------------------------*\
| Namespace
\*----------------------------------------------------------*/

using namespace std;
using namespace kinematics;
using namespace moveit::core;
using namespace robot_state;
using namespace moveit_msgs;
using namespace Eigen;
using namespace str1ker;

/*----------------------------------------------------------*\
| Variables
\*----------------------------------------------------------*/

const char PLUGIN_NAME[] = "str1ker::ik";
IKPluginRegistrar g_registerIkPlugin;

/*----------------------------------------------------------*\
| IKPlugin implementation
\*----------------------------------------------------------*/

//
// Constructor
//

IKPlugin::IKPlugin() : m_pPlanningGroup(NULL), m_node("~")
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
            auto axis = getJointAxis(pJoint);

            ROS_INFO_NAMED(
                PLUGIN_NAME,
                "Joint\n\t%s: %s %s %s\n\taxis %g %g %g\n\tlimits min %g max %g vel %g",
                jointNames[jointIndex].c_str(),
                "revolute",
                pJoint->isPassive() ? "passive" : "active",
                pJoint->getMimic() != NULL ? "mimic" : "",
                axis.x(),
                axis.y(),
                axis.z(),
                limits.min_position,
                limits.max_position,
                limits.max_velocity);

            m_joints.push_back(pJoint);
        }
        else if (pJoint->getType() == JointModel::PRISMATIC)
        {
            auto limits = pJoint->getVariableBoundsMsg()[0];
            auto axis = getJointAxis(pJoint);

            ROS_INFO_NAMED(
                PLUGIN_NAME,
                "Joint\n\t%s: %s %s %s\n\taxis %g %g %g\n\tlimits min %g max %g vel %g",
                jointNames[jointIndex].c_str(),
                "prismatic",
                pJoint->isPassive() ? "passive" : "active",
                pJoint->getMimic() != NULL ? "mimic" : "",
                axis.x(),
                axis.y(),
                axis.z(),
                limits.min_position,
                limits.max_position,
                limits.max_velocity);

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

    // Initialize state
    m_pState.reset(new robot_state::RobotState(robot_model_));
    m_pState->setToDefaultValues();

    // Advertise marker publisher
    m_markerPub = m_node.advertise<visualization_msgs::Marker>(
        "visualization_marker",
        10);

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
    return false;
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
    if (!validateSeedState(ik_seed_state) || !validateTarget(ik_poses))
    {
        error_code.val = error_code.NO_IK_SOLUTION;
        return false;
    }

    solution = ik_seed_state;

    const LinkModel* pTipLink = getTipLink();
    const JointModel* pMountJoint = getJoint(JointModel::REVOLUTE);
    const JointModel* pShoulderJoint = getJoint(JointModel::REVOLUTE, pMountJoint);
    const JointModel* pElbowJoint = getJoint(JointModel::REVOLUTE, pShoulderJoint);

    Isometry3d targetReference = getTarget(ik_poses);
    Isometry3d shoulderReference = m_pState->getGlobalLinkTransform(
        pShoulderJoint->getChildLinkModel());

    Vector3d upperArm = getLinkLength(
        pShoulderJoint->getChildLinkModel(),
        pElbowJoint->getChildLinkModel());
    Vector3d forearm = getLinkLength(
        pElbowJoint->getChildLinkModel(),
        pElbowJoint->getDescendantJointModels().front()->getChildLinkModel());
    Vector3d shoulderToEffector = getLinkLength(
        pShoulderJoint->getChildLinkModel(),
        pTipLink);
    Vector3d elbowToEffector = getLinkLength(
        pElbowJoint->getChildLinkModel(),
        pTipLink);

    // Calculate mount joint angle
    Vector3d targetOffset = targetReference.translation() - shoulderReference.translation();
    double mountAngle = getAngle(targetOffset.x(), targetOffset.y());
    double mountReference = getAngle(upperArm.x(), upperArm.y());
    double mountOffset = getAngle(shoulderToEffector.x(), shoulderToEffector.y()) - mountReference;
    Isometry3d armRotation = setJointState(pMountJoint, mountAngle - mountReference - mountOffset, solution);

    // Calculate shoulder joint angle
    Vector3d targetToEffectorReference = targetReference.translation() - elbowToEffector;
    Isometry3d localToWorld = armRotation * shoulderReference;
    Vector3d targetLocal = localToWorld.inverse() * targetToEffectorReference;
    Isometry3d shoulderTransform, elbowTransform;

    double shoulderOffset = asin(targetLocal.z() / targetLocal.norm());
    double shoulderRaw = lawOfCosines(upperArm.norm(), forearm.norm(), targetLocal.norm());

    if (targetLocal.z() < MIN_TARGET_HEIGHT)
    {
        shoulderTransform = setJointMinState(pShoulderJoint, solution);
    }
    else if (targetLocal.z() > MAX_TARGET_HEIGHT)
    {
        shoulderTransform = setJointMaxState(pShoulderJoint, solution);
    }
    else
    {
        double effectorOffset = getAngle(elbowToEffector.y(), elbowToEffector.z());
        double shoulderAngle = shoulderOffset - effectorOffset + shoulderRaw;

        ROS_INFO("target in local %g, %g, %g", targetLocal.x(), targetLocal.y(), targetLocal.z());
        ROS_INFO("target angle: %g", shoulderOffset * 180.0 / M_PI);
        ROS_INFO("shoulder angle: %g", shoulderAngle * 180.0 / M_PI);

        shoulderTransform = setJointState(pShoulderJoint, shoulderAngle, solution);
    }

    if (targetLocal.y() < MIN_TARGET_OFFSET)
    {
        elbowTransform = setJointMinState(pElbowJoint, solution);
    }
    else if (targetLocal.y() > MAX_TARGET_OFFSET)
    {
        elbowTransform = setJointMaxState(pElbowJoint, solution);
    }
    else
    {
        // Calculate elbow joint angle
        double elbowAngle = lawOfCosines(forearm.norm(), upperArm.norm(), targetLocal.norm()) - M_PI;
        ROS_INFO("elbow angle: %g", -elbowAngle * 180.0 / M_PI);

        elbowTransform = setJointState(pElbowJoint, elbowAngle, solution);
    }

    // Debug markers
    visualization_msgs::Marker mountMarker, armMarker;

    mountMarker.header.frame_id = armMarker.header.frame_id = "world";
    mountMarker.header.stamp = armMarker.header.stamp = ros::Time::now();
    mountMarker.pose.orientation.x = armMarker.pose.orientation.x = 0.0;
    mountMarker.pose.orientation.y = armMarker.pose.orientation.y = 0.0;
    mountMarker.pose.orientation.z = armMarker.pose.orientation.z = 0.0;
    mountMarker.pose.orientation.w = armMarker.pose.orientation.w = 1.0;
    mountMarker.ns = armMarker.ns = "str1ker/ik";
    mountMarker.type = armMarker.type = visualization_msgs::Marker::LINE_STRIP;
    mountMarker.scale.x = armMarker.scale.x = 0.01;
    mountMarker.scale.y = armMarker.scale.y = 0.01;
    mountMarker.scale.z = armMarker.scale.y = 0.01;

    mountMarker.id = 0;
    mountMarker.color.r = 1.0;
    mountMarker.color.g = 0.0;
    mountMarker.color.b = 1.0;
    mountMarker.color.a = 1.0;
    geometry_msgs::Point mountPointStart;
    mountPointStart.x = shoulderReference.translation().x();
    mountPointStart.y = shoulderReference.translation().y();
    mountPointStart.z = shoulderReference.translation().z();
    geometry_msgs::Point mountPointEnd;
    mountPointEnd.x = targetReference.translation().x();
    mountPointEnd.y = targetReference.translation().y();
    mountPointEnd.z = targetReference.translation().z();
    mountMarker.points.push_back(mountPointStart);
    mountMarker.points.push_back(mountPointEnd);

    armMarker.id = 1;
    armMarker.color.r = 0.0;
    armMarker.color.g = 1.0;
    armMarker.color.b = 1.0;
    armMarker.color.a = 1.0;
    geometry_msgs::Point elbowPoint;
    elbowPoint.z = asin(shoulderOffset + shoulderRaw) * upperArm.norm();
    elbowPoint.y = sqrt(pow(upperArm.norm(), 2) - pow(elbowPoint.z, 2));
    elbowPoint.x = 0;
    geometry_msgs::Point effectorPoint;
    effectorPoint.x = targetLocal.x();
    effectorPoint.y = targetLocal.y();
    effectorPoint.z = targetLocal.z();
    armMarker.points.push_back(mountPointStart);
    armMarker.points.push_back(elbowPoint);
    armMarker.points.push_back(effectorPoint);

    m_markerPub.publish(mountMarker);
    m_markerPub.publish(armMarker);

    // Return solution
    error_code.val = error_code.SUCCESS;

    if(!solution_callback.empty())
        solution_callback(ik_poses.front(), solution, error_code);

    return true;
}

//
// Private methods
//

const JointModel* IKPlugin::getJoint(JointModel::JointType type, const JointModel* parent) const
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

const LinkModel* IKPlugin::getTipLink() const
{
    return m_pPlanningGroup->getLinkModel(tip_frames_.front());
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

Isometry3d IKPlugin::getTarget(const vector<geometry_msgs::Pose>& ik_poses) const
{
    Isometry3d target;
    auto targetPose = ik_poses.back();
    tf2::fromMsg(targetPose, target);

    ROS_INFO_NAMED(
        PLUGIN_NAME,
        "IK target %s: %g, %g, %g",
        tip_frames_.front().c_str(),
        targetPose.position.x,
        targetPose.position.y,
        targetPose.position.z
    );

    return target;
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

Vector3d IKPlugin::getLinkLength(const LinkModel* pBaseLink, const LinkModel* pTipLink) const
{
    const Isometry3d& baseLinkPos = m_pState->getGlobalLinkTransform(pBaseLink);
    const Isometry3d& tipLinkPos = m_pState->getGlobalLinkTransform(pTipLink);

    return (tipLinkPos.translation() - baseLinkPos.translation());
}

//
// Static methods
//

double IKPlugin::getAngle(double x, double y)
{
    return atan2(y, x);
}

double IKPlugin::lawOfCosines(double a, double b, double c)
{
    return acos((a * a + b * b - c * c) / (2 * a * b));
}

Isometry3d IKPlugin::setJointState(
    const JointModel* pJoint,
    double angle,
    std::vector<double>& states) const
{
    const Vector3d& axis = getJointAxis(pJoint);
    const JointLimits& limits = pJoint->getVariableBoundsMsg().front();
    Isometry3d transform = Isometry3d(AngleAxisd(angle, axis));

    if (isnan(angle)) return transform;

    double state;
    pJoint->computeVariablePositions(transform, &state);

    size_t index = find(m_joints.begin(), m_joints.end(), pJoint) - m_joints.begin();
    states[index] = clamp(state, limits.min_position, limits.max_position);

    ROS_INFO_NAMED(
        PLUGIN_NAME,
        "IK solution %s:\n\tangle %g\n\taxis [%g %g %g]\n\tstate %g, clamped %g [%g -> %g]",
        pJoint->getName().c_str(),
        angle,
        axis.x(),
        axis.y(),
        axis.z(),
        state,
        states[index],
        limits.min_position,
        limits.max_position
    );

    if (pJoint->getMimic())
    {
        const JointModel* pMimicJoint = pJoint->getMimic();
        const JointLimits& mimicLimits = pMimicJoint->getVariableBoundsMsg().front();
        double mimicState = (state - pJoint->getMimicOffset()) / pJoint->getMimicFactor();

        size_t mimicIndex = find(m_joints.begin(), m_joints.end(), pMimicJoint) - m_joints.begin();
        states[mimicIndex] = clamp(mimicState, mimicLimits.min_position, mimicLimits.max_position);

        ROS_INFO_NAMED(PLUGIN_NAME, "Updating mimic %s: %g from %g", pMimicJoint->getName().c_str(), mimicState, state);
    }

    return transform;
}

Isometry3d IKPlugin::setJointMinState(const JointModel* pJoint, vector<double>& states) const
{
    double jointState = pJoint->getVariableBoundsMsg().front().min_position;
    size_t jointIndex = find(m_joints.begin(), m_joints.end(), pJoint) - m_joints.begin();
    states[jointIndex] = jointState;

    Isometry3d transform;
    pJoint->computeTransform(&jointState, transform);

    return transform;
}

Isometry3d IKPlugin::setJointMaxState(const JointModel* pJoint, vector<double>& states) const
{
    double jointState = pJoint->getVariableBoundsMsg().front().max_position;
    size_t jointIndex = find(m_joints.begin(), m_joints.end(), pJoint) - m_joints.begin();
    states[jointIndex] = jointState;

    Isometry3d transform;
    pJoint->computeTransform(&jointState, transform);

    return transform;
}

const Vector3d& IKPlugin::getJointAxis(const JointModel* pJoint)
{
    if (pJoint->getType() == JointModel::REVOLUTE)
        return dynamic_cast<const RevoluteJointModel*>(pJoint)->getAxis();
    else
        return dynamic_cast<const PrismaticJointModel*>(pJoint)->getAxis();
}

/*----------------------------------------------------------*\
| IKPluginRegistrar implementation
\*----------------------------------------------------------*/

IKPluginRegistrar::IKPluginRegistrar()
{
    // PLUGINLIB_EXPORT_CLASS macro does not compile on Noetic
    class_loader::impl::registerPlugin<str1ker::IKPlugin, kinematics::KinematicsBase>(
        "str1ker::IKPlugin", "kinematics::KinematicsBase");
}
