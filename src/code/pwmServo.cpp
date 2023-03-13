/*
                                                                                     ███████                  
 ████████████  ████████████   ████████████       █  █████████████  █           █  ███       ███  ████████████ 
█              █ █           █            █    █ █  █              █        ███      ███████    █            █
 ████████████  █   █         █████████████   █   █   █             █   █████      ███       ███ █████████████ 
             █ █     █       █            █      █    █            ████      █                  █            █
 ████████████  █       █     █            █      █      █████████  █          █   ███       ███ █            █
                                                                                     ███████                  
 pwmServo.cpp

 PWM Servo Controller Implementation
 Created 1/19/2021

 Uses pigpiod C interface: https://abyz.me.uk/rpi/pigpio/pdif2.html

 Copyright (C) 2021 Valeriy Novytskyy
 This software is licensed under GNU GPLv3
*/

/*----------------------------------------------------------*\
| Includes
\*----------------------------------------------------------*/

#include <math.h>
#include <ros/ros.h>
#include "robot.h"
#include "pwmServo.h"
#include "controllerFactory.h"

/*----------------------------------------------------------*\
| Namespace
\*----------------------------------------------------------*/

using namespace std;
using namespace str1ker;

/*----------------------------------------------------------*\
| Constants
\*----------------------------------------------------------*/

const char pwmServo::TYPE[] = "pwmServo";

/*----------------------------------------------------------*\
| pwmServo implementation
\*----------------------------------------------------------*/

REGISTER_CONTROLLER(pwmServo)

pwmServo::pwmServo(robot& robot, const char* path):
    servo(robot, path),
    m_topic("/robot/pwm"),
    m_lpwm(0),
    m_rpwm(1),
    m_min(1.0),
    m_max(1.0)
{
}

const char* pwmServo::getType()
{
    return pwmServo::TYPE;
}

bool pwmServo::init(ros::NodeHandle node)
{
    if (!m_enable) return true;

    m_pub = node.advertise<Pwm>(getPath(), QUEUE_SIZE);

    if (m_encoder && !m_encoder->init(node))
    {
        ROS_ERROR("  failed to initialize %s encoder", getPath());
        return false;
    }

    ROS_INFO("  initialized %s %s on %s %d LPWM %d RPWM",
        getPath(), getType(), m_topic.c_str(), m_lpwm, m_rpwm);

    return true;
}

double pwmServo::getPos()
{
    return m_encoder ? m_encoder->getPos() : 0.0;
}

void pwmServo::setPos(double target)
{
    // TODO: need to advertise a service for this (what does MoveIt need?)
    if (!m_enable) return;

    ros::Rate rate(4);

    double pos = getPos();
    double lastPos = pos;
    double initialDistance = abs(target - pos);
    double direction = (target - pos) >= 0.0 ? 1.0 : -1.0;
    double distance = 0.0;

    // Start moving
    if (!setVelocity(m_min * direction)) return;

    do
    {
        // Wait until servo reaches position
        rate.sleep();
        pos = getPos();
        lastPos = pos;
        distance = direction > 0 ? target - pos : pos - target;

        // Ramp speed
        double ramp = max(distance, 0.0) / initialDistance;
        double speed = rampSpeed(ramp);
        setVelocity(speed * direction);

#ifdef DEBUG
    if (abs(lastPos - pos) > 0.02)
        ROS_INFO("%g -> %g, distance %g, speed %g, ramp %g", pos, target, distance, speed, ramp);
#endif

    } while (distance > 0);

    // Stop
    setVelocity(0.0);

    // Allow time before next command
    sleep(1);
}

double pwmServo::getMinSpeed()
{
    return m_min;
}

double pwmServo::getMaxSpeed()
{
    return m_max;
}

double pwmServo::rampSpeed(double ramp)
{
    const double RAMP[] = { 0.0, 1.0, 0.0 };
    int index = int(sizeof(RAMP) / sizeof(double) * ramp);
    return m_min + RAMP[index] * (m_max - m_min);
}

double pwmServo::getVelocity()
{
    return m_velocity;
}

bool pwmServo::setVelocity(double velocity)
{
    uint8_t dutyCycle = uint8_t(abs(velocity) * double(DUTY_CYCLE));

    Pwm msg;

    // Request RPWM
    msg.channel1 = m_rpwm;
    msg.dutyCycle1 = velocity >= 0 ? dutyCycle : 0;

    // Request LPWM
    msg.channel2 = m_lpwm;
    msg.dutyCycle2 = velocity >= 0 ? 0 : dutyCycle;

    m_pub.publish(msg);

    m_velocity = velocity;
    setLastError(NULL);
    return true;
}

void pwmServo::deserialize(ros::NodeHandle node)
{
    servo::deserialize(node);

    if (!ros::param::get(getControllerPath("topic"), m_topic))
        ROS_WARN("%s no PWM topic specified, default /robot/pwm", getPath());

    if (!ros::param::get(getControllerPath("lpwm"), m_lpwm))
        ROS_WARN("%s no LPWM channel specified, default 0", getPath());

    if (!ros::param::get(getControllerPath("rpwm"), m_lpwm))
        ROS_WARN("%s no RPWM channel specified, default 1", getPath());

    ros::param::get(getControllerPath("minSpeed"), m_min);
    ros::param::get(getControllerPath("maxSpeed"), m_max);

    m_encoder = shared_ptr<potentiometer>(controllerFactory::deserialize<potentiometer>(
        m_robot, getPath(), "encoder", node));

    if (!m_encoder)
        ROS_WARN("%s failed to load encoder", getPath());
}

controller* pwmServo::create(robot& robot, const char* path)
{
    return new pwmServo(robot, path);
}
