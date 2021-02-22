/*
                                                                                     @@@@@@@                  
 @@@@@@@@@@@@  @@@@@@@@@@@@   @@@@@@@@@@@@       @  @@@@@@@@@@@@@  @           @  @@@       @@@  @@@@@@@@@@@@ 
@              @ @           @            @    @ @  @              @        @@@      @@@@@@@    @            @
 @@@@@@@@@@@@  @   @         @@@@@@@@@@@@@   @   @   @             @   @@@@@      @@@       @@@ @@@@@@@@@@@@@ 
             @ @     @       @            @      @    @@           @@@@      @                  @            @
 @@@@@@@@@@@@  @       @     @            @      @      @@@@@@@@@  @          @   @@@       @@@ @            @
                                                                                     @@@@@@@                  
 robot.cpp

 Robot controller implementation
 Created 11/28/2020

 This software is licensed under GNU GPLv3
*/

/*----------------------------------------------------------*\
| Includes
\*----------------------------------------------------------*/

#include <vector>
#include <set>
#include <pigpio.h>
#include <ros/ros.h>
#include "robot.h"
#include "controllerFactory.h"

/*----------------------------------------------------------*\
| Namespace
\*----------------------------------------------------------*/

using namespace str1ker;
using namespace std;

/*----------------------------------------------------------*\
| Constants
\*----------------------------------------------------------*/

const char robot::PATH[] = "/robot";

/*----------------------------------------------------------*\
| robot implementation
\*----------------------------------------------------------*/

robot::robot()
{
}

robot::~robot()
{
    for(armArray::iterator pos = m_arms.begin();
        pos != m_arms.end();
        ++pos)
    {
        delete *pos;
    }
}

arm* robot::getArm(int index)
{
    return m_arms[index];
}

arm* robot::getArm(const char* name)
{
    return m_armNames[name];
}

bool robot::init()
{
    ROS_INFO("initializing controllers...");

    if (gpioInitialise() < 0)
    {
        ROS_ERROR("failed to initialize GPIO");
        return false;
    }

    for (armArray::iterator pos = m_arms.begin();
        pos != m_arms.end();
        pos++)
    {
        if (!(*pos)->init())
            return false;
    }

    if (m_adc && !m_adc->init()) return false;

    ROS_INFO("initialization completed");

    return true;
}

void robot::publish()
{
    for(armArray::iterator pos = m_arms.begin();
        pos != m_arms.end();
        ++pos)
    {
        (*pos)->publish();
    }
}

robot& robot::deserialize(ros::NodeHandle node)
{
    ROS_INFO("loading controllers...");

    m_adc = controllerFactory::deserialize<adc>("/robot", "adc", node);

    deserializeArms(node);

    ROS_INFO("loaded successfully");

    return *this;
}

void robot::deserializeArms(ros::NodeHandle node)
{
    vector<string> params;
    set<string> unique;
    char armPath[255] = {0};

    ros::param::getParamNames(params);

    for (vector<string>::iterator pos = params.begin();
        pos != params.end();
        pos++)
    {
        const char* path = getControllerPath(pos->c_str(), "arms", armPath);

        if (path && unique.find(path) == unique.end())
        {
            unique.insert(armPath);

            arm* robotArm = new arm(path);
            robotArm->deserialize(node);

            m_armNames[getControllerName(path)] = robotArm;
            m_arms.push_back(robotArm);
        }
    }
}

const char* robot::getControllerName(const char* path)
{
    const char* lastSep = strrchr(path, '/');

    if (lastSep == NULL) return path;

    return lastSep + 1;
}

const char* robot::getControllerPath(const char* path, const char* componentType, char* componentPath)
{
    char componentTypePath[64] = {0};
    sprintf(componentTypePath, "/%s/", componentType);

    const char* typeNode = strstr(path, componentTypePath);
    if (typeNode == NULL) return NULL;

    const char* nameNode = typeNode + strlen(componentTypePath);
    const char* nextNode = strchr(nameNode , '/');

    if (nextNode == NULL) nextNode = nameNode + strlen(nameNode);

    strncpy(componentPath, path, nextNode - path);
    return componentPath;
}

robot& robot::logo()
{
    puts("                                                                                     @@@@@@@                  ");
    puts(" @@@@@@@@@@@@  @@@@@@@@@@@@   @@@@@@@@@@@@       @  @@@@@@@@@@@@@  @           @  @@@       @@@  @@@@@@@@@@@@ ");
    puts("@              @ @           @            @    @ @  @              @        @@@      @@@@@@@    @            @");
    puts(" @@@@@@@@@@@@  @   @         @@@@@@@@@@@@@   @   @   @             @   @@@@@      @@@       @@@ @@@@@@@@@@@@@ ");
    puts("             @ @     @       @            @      @    @@           @@@@      @                  @            @");
    puts(" @@@@@@@@@@@@  @       @     @            @      @      @@@@@@@@@  @          @   @@@       @@@ @            @");
    puts("                                                                                     @@@@@@@                  ");

    return *this;
}