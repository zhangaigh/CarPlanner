/*
 * File:   Localizer.cpp
 * Author: jmf
 *
 * Created on February 16, 2012, 5:07 PM
 */

#include "Localizer.h"

#include <math.h>

#include "CarPlannerCommon.h"
#include "MochaException.h"
#include "RpgUtils.h"

/// MUST set up a Posys object using the command:
/// PoseToNode -posys vicon://IP_address_to_vicon_machine:[NinjaCar]
///
/// When running PoseToNode, do NOT use a different node name; it is
/// hard coded to be `posetonode'.

//////////////////////////////////////////////////////////////////
Localizer::Localizer()
{
    m_bIsStarted = false;
    m_pNode = new node::node;
    m_pNode->init("commander_node");
    if( m_pNode->advertise("command") == false ){
      LOG(ERROR) << "Error setting up publisher.";
    }

}

//////////////////////////////////////////////////////////////////
void Localizer::TrackObject(
        const std::string& sObjectName,
        const std::string& sHost,
        bool bRobotFrame /*= true*/
        ){
    TrackObject(sObjectName,sHost,Sophus::SE3d(),bRobotFrame);
}

//////////////////////////////////////////////////////////////////
void Localizer::TrackObject(
        const std::string& sObjectName,
        const std::string& sHost,
        Sophus::SE3d dToffset,
        bool bRobotFrame /*= true*/
        )
{
    std::string sUri = sHost + "/" + sObjectName ;

    TrackerObject* pObj = &m_mObjects[ sObjectName ];

    pObj->m_bNodeSubscribed = false;

    if( !pObj->m_bNodeSubscribed ) {
      if( m_pNode->subscribe( sUri ) == false ) {
        LOG(ERROR) << "Could not subscribe to " << sUri;
      }
      pObj->m_bNodeSubscribed = true;
    }

    pObj->m_dToffset = dToffset;
    pObj->m_bRobotFrame = bRobotFrame;
    pObj->m_pLocalizerObject = this;
}

//////////////////////////////////////////////////////////////////
Localizer::~Localizer()
{
  delete m_pNode;
}

//////////////////////////////////////////////////////////////////
void Localizer::Start()
{
    if( m_bIsStarted == true ) {
        throw MochaException("The Localizer thread has already started.");
    }

    m_pThread = new boost::thread([this] () { Localizer::_ThreadFunction(this); } );
    m_bIsStarted = true;
}

//////////////////////////////////////////////////////////////////
void Localizer::Stop()
{
    if( m_bIsStarted == false ) {
        //throw MochaException("No thread is running!");
        return;
    }

    m_pThread->interrupt();
    m_pThread->join();

    m_bIsStarted = false;
}

//////////////////////////////////////////////////////////////////
//
Sophus::SE3d Localizer::GetPose( const std::string& sObjectName, bool blocking/* = false */,
                                          double* time /*= NULL*/, double* rate /*= NULL*/)
{
    if( m_mObjects.find( sObjectName ) == m_mObjects.end() ){
        throw MochaException("Invalid object name.");
    }

    TrackerObject& obj = m_mObjects[sObjectName];
    boost::mutex::scoped_lock lock(obj.m_Mutex);

    //if blocking wait until we have a signal that the pose for this object has been updated
    if(blocking && obj.m_bPoseUpdated == false){
        obj.m_PoseUpdated.wait(lock);
    }
    obj.m_bPoseUpdated = false;

    Sophus::SE3d pose = m_mObjects[sObjectName].m_dSensorPose;
    if(time != NULL){
        *time = m_mObjects[sObjectName].m_dTime;
    }
    if(rate != NULL){
        *rate = m_mObjects[sObjectName].m_dPoseRate;
    }
    return pose;
}

//////////////////////////////////////////////////////////////////
//
//Eigen::Matrix<double,6,1> Localizer::GetdPose( const std::string& sObjectName )
//{
//    if( m_mObjects.find( sObjectName ) == m_mObjects.end() ){
//        throw MochaException("Invalid object name.");
//    }

//    //lock the sensor pose for readout
//    lock();
//    Eigen::Matrix<double,6,1> pose = m_mObjects[sObjectName].m_dDSensorPose;
//    unlock();

//    return pose;
//}

//////////////////////////////////////////////////////////////////
eLocType Localizer::WhereAmI( Eigen::Vector3d P )
{
    return VT_AIR;
}

//////////////////////////////////////////////////////////////////
eLocType Localizer::WhereAmI( Eigen::Matrix<double, 6, 1 > P )
{
    Eigen::Vector3d Pv = P.block < 3, 1 > (0, 0);
    return WhereAmI( Pv );
}

//////////////////////////////////////////////////////////////////
void Localizer::_ThreadFunction(Localizer *pV) {
  while (1) {
    std::map< std::string, TrackerObject >::iterator it;
    for( it = pV->m_mObjects.begin(); it != pV->m_mObjects.end(); it++ ) {

      it->second.m_bNodeSubscribed = false;

      // Subscribe to the Posys node.
      if( !it->second.m_bNodeSubscribed ) {
        if( m_pNode->subscribe( it->first ) == false ) {
          LOG(ERROR) << "Could not subscribe to " << it->first;
        }
        it->second.m_bNodeSubscribed = true;
      }

      hal::PoseMsg posys;

      if( m_pNode->receive( it->first, posys ) ) {
        if(posys.type() == hal::PoseMsg::Type::PoseMsg_Type_SE3) {
          DLOG(INFO) << "Received Posys message with data: "
                     << posys.pose().data(0) << " "
                     << posys.pose().data(1) << " "
                     << posys.pose().data(2);
        } else {
          LOG(ERROR) << "Incorrect Posys message type.";
        }
      } else if( m_pNode->subscribe( it->first ) == false ) {
        LOG(INFO) << "Could not re-subscribe to " << it->first;
        it->second.m_bNodeSubscribed = false;
      } else {
        LOG(INFO) << "Did not get a message.";
      }

      {
        boost::mutex::scoped_lock lock(it->second.m_Mutex);

        Eigen::Matrix4d T;
        if(it->second.m_bRobotFrame){
          T << 1, 0, 0, 0,
              0, -1, 0, 0,
              0, 0, -1, 0,
              0, 0 , 0, 1;
        } else {
          T = Eigen::Matrix4d::Identity();
        }

          Eigen::Vector3d Pos(posys.pose().data(0), posys.pose().data(1), posys.pose().data(2));
          Eigen::Quaterniond Quat(posys.pose().data(3), posys.pose().data(4),
              posys.pose().data(5), posys.pose().data(6));

          //get the pose and transform it as necessary
          Sophus::SE3d Twc( Sophus::SO3d(Quat) ,Pos);

          it->second.m_dSensorPose = Sophus::SE3d(T) * (it->second.m_dToffset * Twc);

          //now calculate the time derivative
          //used to be the next line, but now is modified for hal::PosysMsg.
          //double localizerTime = tData.msg_time.tv_sec + 1e-6*tData.msg_time.tv_usec;
          double localizerTime = posys.device_time();

          //calculate metrics
          if(it->second.m_dLastTime == -1){
            it->second.m_dLastTime = localizerTime;
            it->second.m_nNumPoses = 0;
            it->second.m_dPoseRate = 0;
          }else if((localizerTime - it->second.m_dLastTime) > 1){
            it->second.m_dPoseRate = it->second.m_nNumPoses /(localizerTime - it->second.m_dLastTime) ;
            it->second.m_dLastTime = localizerTime;
            it->second.m_nNumPoses = 0;
          }
          it->second.m_nNumPoses++;
          it->second.m_dTime = localizerTime;

      }

      //signal that the object has been update
      it->second.m_bPoseUpdated = true;
      it->second.m_PoseUpdated.notify_all();
    }

    boost::this_thread::interruption_point();
  }

  //small sleep to not eat up all the cpu
  usleep(1000);
}
