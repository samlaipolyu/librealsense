// License: Apache 2.0. See LICENSE file in root directory.
// Copyright(c) 2017 Intel Corporation. All Rights Reserved.

#include "RsRtspClient.h"
#include <ipDeviceCommon/RsCommon.h>

#include "liveMedia.hh"
#include "BasicUsageEnvironment.hh"

#include <iostream>
#include <thread>
//#include <condition_variable>
#include <vector>
#include <string>
#include <math.h>
#include <algorithm>

#define RTSP_CLIENT_VERBOSITY_LEVEL 0 // by default, print verbose output from each "RTSPClient"
#define REQUEST_STREAMING_OVER_TCP 0  // TODO - uderstand this

std::string format_error_msg(std::string function, RsRtspReturnValue retVal)
{
  return std::string("[" + function + "] error: " + retVal.msg + " - " + std::to_string(retVal.exit_code));
}

long long int RsRTSPClient::getStreamProfileUniqueKey(rs2_video_stream t_profile)
{
  long long int key;
  key = t_profile.type * pow(10, 12) + t_profile.fmt * pow(10, 10) + t_profile.fps * pow(10, 8) + t_profile.index + t_profile.width * pow(10, 4) + t_profile.height;
  return key;
}

//TODO: change char* to std::string
IRsRtsp *RsRTSPClient::getRtspClient(char const *t_rtspURL,
                                     char const *t_applicationName, portNumBits t_tunnelOverHTTPPortNum)
{
  TaskScheduler *scheduler = BasicTaskScheduler::createNew();
  UsageEnvironment *env = BasicUsageEnvironment::createNew(*scheduler);
  
  RTSPClient::responseBufferSize = 100000;
  return (IRsRtsp *)new RsRTSPClient(scheduler,env, t_rtspURL, RTSP_CLIENT_VERBOSITY_LEVEL, t_applicationName, t_tunnelOverHTTPPortNum);
  //return rtspClient;
}

RsRTSPClient::RsRTSPClient(TaskScheduler *t_scheduler,UsageEnvironment *t_env, char const *t_rtspURL,
                           int t_verbosityLevel, char const *t_applicationName, portNumBits t_tunnelOverHTTPPortNum)
    : RTSPClient(*t_env, t_rtspURL, t_verbosityLevel, t_applicationName, t_tunnelOverHTTPPortNum, -1)
{
  m_lastReturnValue.exit_code=RsRtspReturnCode::OK;
  m_env = t_env;
  m_scheduler = t_scheduler;
}

RsRTSPClient::~RsRTSPClient()
{
}

std::vector<rs2_video_stream> RsRTSPClient::getStreams()
{
  // TODO - handle in a function
  unsigned res = this->sendDescribeCommand(this->continueAfterDESCRIBE);
  if (res == 0)
  {
    // An error occurred (continueAfterDESCRIBE was already called)
    // TODO: return error code
    return this->m_supportedProfiles;
  }
  // wait for continueAfterDESCRIBE to finish
  std::unique_lock<std::mutex> lck(m_commandMtx);
  m_cv.wait_for(lck, std::chrono::seconds(RTSP_CLIENT_COMMANDS_TIMEOUT_SEC), [this] { return m_commandDone; });
  // for the next command - if not done - throw timeout
  if(!m_commandDone)
  {
    RsRtspReturnValue err = 
      {RsRtspReturnCode::ERROR_TIME_OUT,"client time out"};
    throw std::runtime_error(format_error_msg(__FUNCTION__, err));
  }
  m_commandDone = false;
  
  if (m_lastReturnValue.exit_code!=RsRtspReturnCode::OK)
  {
    throw std::runtime_error(format_error_msg(__FUNCTION__, m_lastReturnValue));
  }

  if (this->m_supportedProfiles.size()==0)
  {
    RsRtspReturnValue err = 
      {RsRtspReturnCode::ERROR_GENERAL, std::string("failed to get streams from network device at url: " + std::string(this->url()))};
    throw std::runtime_error(format_error_msg(__FUNCTION__, err));
  }

  return this->m_supportedProfiles;
}

int RsRTSPClient::addStream(rs2_video_stream t_stream, rtp_callback *t_callbackObj)
{
  long long int uniqueKey = getStreamProfileUniqueKey(t_stream);
  RsMediaSubsession *subsession = this->m_subsessionMap.find(uniqueKey)->second;
  if(subsession == nullptr)
  {
    RsRtspReturnValue err = 
      {RsRtspReturnCode::ERROR_WRONG_FLOW,"requested stream was not found"};
    throw std::runtime_error(format_error_msg(__FUNCTION__, err));
  }
  
    if (!subsession->initiate())
    {
      this->envir() << "Failed to initiate the subsession \n";
        RsRtspReturnValue err = {RsRtspReturnCode::ERROR_WRONG_FLOW,"Failed to initiate the subsession"};
      throw std::runtime_error(format_error_msg(__FUNCTION__,err));
    }
    else
    {
      // Continue setting up this subsession, by sending a RTSP "SETUP" command:
      unsigned res = this->sendSetupCommand(*subsession, this->continueAfterSETUP, False, REQUEST_STREAMING_OVER_TCP);
      // wait for continueAfterSETUP to finish
      std::unique_lock<std::mutex> lck(m_commandMtx);
      m_cv.wait_for(lck, std::chrono::seconds(RTSP_CLIENT_COMMANDS_TIMEOUT_SEC), [this] { return m_commandDone; });
      // for the next command
      if(!m_commandDone)
      {
      RsRtspReturnValue err = 
        {RsRtspReturnCode::ERROR_TIME_OUT,"client time out"};
      throw std::runtime_error(format_error_msg(__FUNCTION__, err));
      }
      m_commandDone = false;

      if(m_lastReturnValue.exit_code!=RsRtspReturnCode::OK)
      {
        throw std::runtime_error(format_error_msg(__FUNCTION__,m_lastReturnValue));
      }
      else
      {
        subsession->sink = RsSink::createNew(this->envir(), *subsession, t_stream, m_memPool, this->url());
        // perhaps use your own custom "MediaSink" subclass instead
        if (subsession->sink == NULL)
        {
          this->envir() << "Failed to create a data sink for the subsession: " << this->envir().getResultMsg() << "\n";
          RsRtspReturnValue err = 
            {(RsRtspReturnCode)envir().getErrno(),std::string("Failed to create a data sink for the subsession: " + std::string(envir().getResultMsg()))};
          throw std::runtime_error(format_error_msg(__FUNCTION__,err));
          // TODO: define error
        }

        subsession->miscPtr = this; // a hack to let subsession handler functions get the "RTSPClient" from the subsession
        ((RsSink *)(subsession->sink))->setCallback(t_callbackObj);
        subsession->sink->startPlaying(*(subsession->readSource()),
                                       subsessionAfterPlaying, subsession);
        // Also set a handler to be called if a RTCP "BYE" arrives for this subsession:
        if (subsession->rtcpInstance() != NULL)
        {
          subsession->rtcpInstance()->setByeWithReasonHandler(subsessionByeHandler, subsession);
        }
      }
      return this->m_lastReturnValue.exit_code;
    }
}

int RsRTSPClient::start()
{
  unsigned res = this->sendPlayCommand(*this->m_scs.m_session, this->continueAfterPLAY);
  // wait for continueAfterPLAY to finish
  std::unique_lock<std::mutex> lck(m_commandMtx);
  m_cv.wait_for(lck, std::chrono::seconds(RTSP_CLIENT_COMMANDS_TIMEOUT_SEC), [this] { return m_commandDone; });
  // for the next command
  if(!m_commandDone)
  {
    RsRtspReturnValue err = 
      {RsRtspReturnCode::ERROR_TIME_OUT,"client time out"};
    throw std::runtime_error(format_error_msg(__FUNCTION__, err));
  }
  m_commandDone = false;

  if(m_lastReturnValue.exit_code!=RsRtspReturnCode::OK)
  {
    throw std::runtime_error(format_error_msg(__FUNCTION__,m_lastReturnValue));
  }
  return m_lastReturnValue.exit_code;
}

int RsRTSPClient::stop()
{
  unsigned res = this->sendPauseCommand(*this->m_scs.m_session, this->continueAfterPAUSE);
  // wait for continueAfterPAUSE to finish
  std::unique_lock<std::mutex> lck(m_commandMtx);
  m_cv.wait_for(lck, std::chrono::seconds(RTSP_CLIENT_COMMANDS_TIMEOUT_SEC), [this] { return m_commandDone; });
  // for the next command
  if(!m_commandDone)
  {
    RsRtspReturnValue err = 
      {RsRtspReturnCode::ERROR_TIME_OUT,"client time out"};
    throw std::runtime_error(format_error_msg(__FUNCTION__, err));
  }
  m_commandDone = false;
  if(m_lastReturnValue.exit_code!=RsRtspReturnCode::OK)
  {
    throw std::runtime_error(format_error_msg(__FUNCTION__,m_lastReturnValue));
  }
  return m_lastReturnValue.exit_code;
}

int RsRTSPClient::close()
{
  unsigned res = this->sendTeardownCommand(*this->m_scs.m_session, this->continueAfterTEARDOWN);
  // wait for continueAfterTEARDOWN to finish
  std::unique_lock<std::mutex> lck(m_commandMtx);
  m_cv.wait_for(lck, std::chrono::seconds(RTSP_CLIENT_COMMANDS_TIMEOUT_SEC), [this] { return m_commandDone; });
  // for the next command
  if(!m_commandDone)
  {
    RsRtspReturnValue err = 
      {RsRtspReturnCode::ERROR_TIME_OUT,"client time out"};
    throw std::runtime_error(format_error_msg(__FUNCTION__, err));
  }
  m_commandDone = false;

  if(m_lastReturnValue.exit_code!=RsRtspReturnCode::OK)
  {
    throw std::runtime_error(format_error_msg(__FUNCTION__,m_lastReturnValue));
  }
  m_eventLoopWatchVariable = ~0;
  std::unique_lock<std::mutex> lk(m_taskSchedulerMutex);
  this->envir() << "Closing the stream.\n";
  Medium::close(this);
  m_env->reclaim(); m_env = NULL;
  delete m_scheduler; m_scheduler = NULL;
  return m_lastReturnValue.exit_code;
}

int RsRTSPClient::setOption(rs2_option t_opt, float t_val)
{
  std::string option = std::to_string(t_opt);
  std::string value = std::to_string(t_val);
  unsigned res = this->sendSetParameterCommand(*this->m_scs.m_session, this->continueAfterSETCOMMAND, option.c_str(), value.c_str());
  // wait for continueAfterPLAY to finish
  std::unique_lock<std::mutex> lck(m_commandMtx);
  m_cv.wait_for(lck, std::chrono::seconds(RTSP_CLIENT_COMMANDS_TIMEOUT_SEC), [this] { return m_commandDone; });
  // for the next command
  if(!m_commandDone)
  {
    RsRtspReturnValue err = 
      {RsRtspReturnCode::ERROR_TIME_OUT,"client time out"};
    throw std::runtime_error(format_error_msg(__FUNCTION__, err));
  }
  m_commandDone = false;
  /*
  TODO: enable after fixing the option flow
  if(m_lastReturnValue.exit_code!=RsRtspReturnCode::OK)
  {
    throw std::runtime_error(format_error_msg(__FUNCTION__,m_lastReturnValue));
  }
  */
  t_val = m_getParamRes;
  return m_lastReturnValue.exit_code;
}

void RsRTSPClient::setGetParamResponse(float t_res)
{
  m_getParamRes = t_res;
}

int RsRTSPClient::getOption(rs2_option t_opt, float &t_val)
{
  unsigned res = this->sendGetParameterCommand(*this->m_scs.m_session, this->continueAfterGETCOMMAND, (const char *)&t_opt);
  // wait for continueAfterPLAY to finish
  std::unique_lock<std::mutex> lck(m_commandMtx);
  m_cv.wait_for(lck, std::chrono::seconds(RTSP_CLIENT_COMMANDS_TIMEOUT_SEC), [this] { return m_commandDone; });
  // for the next command
  if(!m_commandDone)
  {
    RsRtspReturnValue err = 
      {RsRtspReturnCode::ERROR_TIME_OUT,"client time out"};
    throw std::runtime_error(format_error_msg(__FUNCTION__, err));
  }
  m_commandDone = false;
  if(m_lastReturnValue.exit_code!=RsRtspReturnCode::OK)
  {
    throw std::runtime_error(format_error_msg(__FUNCTION__,m_lastReturnValue));
  }
  return m_lastReturnValue.exit_code;
}

void schedulerThread(RsRTSPClient *t_rtspClientInstance)
{
  std::unique_lock<std::mutex> lk(t_rtspClientInstance->getTaskSchedulerMutex());
  t_rtspClientInstance->envir().taskScheduler().doEventLoop(&t_rtspClientInstance->getEventLoopWatchVariable());
  lk.unlock();
}

void RsRTSPClient::initFunc(MemoryPool *t_pool)
{
  std::thread thread_scheduler(schedulerThread, this);
  thread_scheduler.detach();
  m_memPool = t_pool;
}

void RsRTSPClient::setDeviceData(DeviceData t_data)
{
  m_deviceData = t_data;
}
std::vector<IpDeviceControlData> controls;

std::vector<IpDeviceControlData> RsRTSPClient::getControls()
{
  this->sendOptionsCommand(this->continueAfterOPTIONS);

  // wait for continueAfterOPTIONS to finish
  std::unique_lock<std::mutex> lck(m_commandMtx);
  m_cv.wait_for(lck, std::chrono::seconds(RTSP_CLIENT_COMMANDS_TIMEOUT_SEC), [this] { return m_commandDone; });
  // for the next command
  if(!m_commandDone)
  {
    RsRtspReturnValue err = 
      {RsRtspReturnCode::ERROR_TIME_OUT,"client time out"};
    throw std::runtime_error(format_error_msg(__FUNCTION__, err));
  }
  m_commandDone = false;
  
  if(m_lastReturnValue.exit_code!=RsRtspReturnCode::OK)
  {
    throw std::runtime_error(format_error_msg(__FUNCTION__,m_lastReturnValue));
  }
  //TODO: return the controls at argument
  return controls;
}

/*********************************
 *          CALLBACKS            *
 *********************************/

// TODO: Error handling
void RsRTSPClient::continueAfterDESCRIBE(RTSPClient *rtspClient, int resultCode, char *resultString)
{
  UsageEnvironment &env = rtspClient->envir();                  // alias
  StreamClientState &scs = ((RsRTSPClient *)rtspClient)->m_scs; // alias
  RsRTSPClient *rsRtspClient = ((RsRTSPClient *)rtspClient);    // alias
  
  if (NULL!=resultString)
    rsRtspClient->m_lastReturnValue.msg = resultString;
  rsRtspClient->m_lastReturnValue.exit_code = (RsRtspReturnCode)resultCode;

  //TODO: take logic out of the callback? 
  do
  {
    if (resultCode != 0)
    {
      env << "Failed to get a SDP description: " << resultString << "\n";
      delete[] resultString;
      break;
    }

    char *const sdpDescription = resultString;

    // Create a media session object from this SDP description:
    scs.m_session = RsMediaSession::createNew(env, sdpDescription);
    delete[] sdpDescription; // because we don't need it anymore
    if (scs.m_session == NULL)
    {
      env << "Failed to create a RsMediaSession object from the SDP description: " << env.getResultMsg() << "\n";
      break;
    }
    else if (!scs.m_session->hasSubsessions())
    {
      env << "This session has no media subsessions (i.e., no \"m=\" lines)\n";
      break;
    }

    RsMediaSubsessionIterator iter(*scs.m_session);
    RsMediaSubsession *subsession = iter.next();
    while (subsession != NULL)
    {
      // Get more data from the SDP string
      const char *strWidthVal = subsession->attrVal_str("width");
      const char *strHeightVal = subsession->attrVal_str("height");
      const char *strFormatVal = subsession->attrVal_str("format");
      const char *strUidVal = subsession->attrVal_str("uid");
      const char *strFpsVal = subsession->attrVal_str("fps");
      const char *strIndexVal = subsession->attrVal_str("stream_index");
      const char *strStreamTypeVal = subsession->attrVal_str("stream_type");
      const char *strBppVal = subsession->attrVal_str("bpp");

      const char *strSerialNumVal = subsession->attrVal_str("cam_serial_num");
      const char *strCamNameVal = subsession->attrVal_str("cam_name");
      const char *strUsbTypeVal = subsession->attrVal_str("usb_type");

      int width = strWidthVal != "" ? std::stoi(strWidthVal) : 0;
      int height = strHeightVal != "" ? std::stoi(strHeightVal) : 0;
      int format = strFormatVal != "" ? std::stoi(strFormatVal) : 0;
      int uid = strUidVal != "" ? std::stoi(strUidVal) : 0;
      int fps = strFpsVal != "" ? std::stoi(strFpsVal) : 0;
      int index = strIndexVal != "" ? std::stoi(strIndexVal) : 0;
      int stream_type = strStreamTypeVal != "" ? std::stoi(strStreamTypeVal) : 0;
      int bpp = strBppVal != "" ? std::stoi(strBppVal) : 0;
      rs2_video_stream videoStream;
      videoStream.width = width;
      videoStream.height = height;
      videoStream.uid = uid;
      videoStream.fmt = static_cast<rs2_format>(format);
      videoStream.fps = fps;
      videoStream.index = index;
      videoStream.type = static_cast<rs2_stream>(stream_type);
      videoStream.bpp = bpp;

      // intrinsics desirialization should happend at once (usgin json?)
      videoStream.intrinsics.width = subsession->attrVal_int("width");
      videoStream.intrinsics.height = subsession->attrVal_int("height");
      videoStream.intrinsics.ppx = subsession->attrVal_int("ppx");
      videoStream.intrinsics.ppy = subsession->attrVal_int("ppy");
      videoStream.intrinsics.fx = subsession->attrVal_int("fx");
      videoStream.intrinsics.fy = subsession->attrVal_int("fy");
      CompressionFactory::getIsEnabled() = subsession->attrVal_bool("compression");
      videoStream.intrinsics.model = (rs2_distortion)subsession->attrVal_int("model");

      // TODO: adjust serialization to camera distortion model
      for (size_t i = 0; i < 5; i++)
      {
        videoStream.intrinsics.coeffs[i] = subsession->attrVal_int("coeff_" + i);
      }

      DeviceData deviceData;
      deviceData.serialNum = strSerialNumVal;
      deviceData.name = strCamNameVal;
      // Return spaces back to string after getting it from the SDP
      // TODO Michal: Decide what character to use for replacing spaces
      std::replace(deviceData.name.begin(), deviceData.name.end(), '^', ' ');
      deviceData.usbType = strUsbTypeVal;
      rsRtspClient->setDeviceData(deviceData);

      // TODO: update width and height in subsession?
      long long int uniqueKey = getStreamProfileUniqueKey(videoStream);
      // TODO Michal: should the map key be long long?
      rsRtspClient->m_subsessionMap.insert(std::pair<long long int, RsMediaSubsession *>(uniqueKey, subsession));
      rsRtspClient->m_supportedProfiles.push_back(videoStream);
      subsession = iter.next();
      // TODO: when to delete p?
    }
  //TODO:remove this loop - once will be at API function?
  } while (0);

  {
    std::lock_guard<std::mutex> lck(rsRtspClient->m_commandMtx);
    rsRtspClient->m_commandDone = true;
  }
  rsRtspClient->m_cv.notify_one();

  // An unrecoverable error occurred with this stream.
  // TODO:
  //shutdownStream(rtspClient);
}

void RsRTSPClient::continueAfterSETUP(RTSPClient *rtspClient, int resultCode, char *resultString)
{
  UsageEnvironment &env = rtspClient->envir();                  // alias
  StreamClientState &scs = ((RsRTSPClient *)rtspClient)->m_scs; // alias
  RsRTSPClient *rsRtspClient = ((RsRTSPClient *)rtspClient);    // alias
  env << "continueAfterSETUP " << resultCode << " " << resultString << "\n";

  if (NULL!=resultString)
    rsRtspClient->m_lastReturnValue.msg = resultString;

  rsRtspClient->m_lastReturnValue.exit_code = (RsRtspReturnCode)resultCode;
  {
    std::lock_guard<std::mutex> lck(rsRtspClient->m_commandMtx);
    rsRtspClient->m_commandDone = true;
  }
  rsRtspClient->m_cv.notify_one();
}

void RsRTSPClient::continueAfterPLAY(RTSPClient *rtspClient, int resultCode, char *resultString)
{
  UsageEnvironment &env = rtspClient->envir();               // alias
  RsRTSPClient *rsRtspClient = ((RsRTSPClient *)rtspClient); // alias
  env << "continueAfterPLAY " << resultCode << " " << resultString << "\n";
  
  if (NULL!=resultString)
    rsRtspClient->m_lastReturnValue.msg = resultString;
  rsRtspClient->m_lastReturnValue.exit_code = (RsRtspReturnCode)resultCode;

  {
    std::lock_guard<std::mutex> lck(rsRtspClient->m_commandMtx);
    rsRtspClient->m_commandDone = true;
  }
  rsRtspClient->m_cv.notify_one();
}

void RsRTSPClient::continueAfterTEARDOWN(RTSPClient *rtspClient, int resultCode, char *resultString)
{
  UsageEnvironment &env = rtspClient->envir();               // alias
  RsRTSPClient *rsRtspClient = ((RsRTSPClient *)rtspClient); // alias
  env << "continueAfterTEARDOWN " << resultCode << " " << resultString << "\n";

  if (NULL!=resultString)
    rsRtspClient->m_lastReturnValue.msg = resultString;
  rsRtspClient->m_lastReturnValue.exit_code = (RsRtspReturnCode)resultCode;

  {
    std::lock_guard<std::mutex> lck(rsRtspClient->m_commandMtx);
    rsRtspClient->m_commandDone = true;
  }
  rsRtspClient->m_cv.notify_one();
}

void RsRTSPClient::continueAfterPAUSE(RTSPClient *rtspClient, int resultCode, char *resultString)
{
  UsageEnvironment &env = rtspClient->envir();               // alias
  RsRTSPClient *rsRtspClient = ((RsRTSPClient *)rtspClient); // alias
  env << "continueAfterPAUSE " << resultCode << " " << resultString << "\n";
  
  if (NULL!=resultString)
    rsRtspClient->m_lastReturnValue.msg = resultString;
  rsRtspClient->m_lastReturnValue.exit_code = (RsRtspReturnCode)resultCode;
  
  {
    std::lock_guard<std::mutex> lck(rsRtspClient->m_commandMtx);
    rsRtspClient->m_commandDone = true;
  }
  rsRtspClient->m_cv.notify_one();
}

void RsRTSPClient::continueAfterOPTIONS(RTSPClient *rtspClient, int resultCode, char *resultString)
{
  UsageEnvironment &env = rtspClient->envir();               // alias
  RsRTSPClient *rsRtspClient = ((RsRTSPClient *)rtspClient); // alias
  env << "continueAfterOPTIONS " << resultCode << " " << resultString << "\n";

  if (NULL!=resultString)
    rsRtspClient->m_lastReturnValue.msg = resultString;
  rsRtspClient->m_lastReturnValue.exit_code = (RsRtspReturnCode)resultCode;
  
  //TODO:move logic from callback
  {
    std::lock_guard<std::mutex> lck(rsRtspClient->m_commandMtx);
    std::string s = (std::string)resultString;
    std::size_t foundBegin = s.find_first_of("[");
    IpDeviceControlData controlData;
    int counter = 0;
    while (foundBegin != std::string::npos)
    {

      std::size_t foundEnd = s.find_first_of("]", foundBegin + 1);
      std::string controlsPerSensor = s.substr(foundBegin + 1, foundEnd - foundBegin);
      std::size_t pos = 0;
      while ((pos = controlsPerSensor.find(';')) != std::string::npos)
      {
        std::string controlStr = controlsPerSensor.substr(0, pos);
        std::size_t pos1 = controlStr.find('{');
        controlData.sensorId = counter == 0 ? 1 : 0;
        controlData.option = (rs2_option)stoi(controlStr.substr(0, pos1));
        std::size_t pos2 = controlStr.find(',', pos1 + 1);
        controlData.range.min = stof(controlStr.substr(pos1 + 1, pos2 - (pos1 + 1)));
        pos1 = controlStr.find(',', pos2 + 1);
        controlData.range.max = stof(controlStr.substr(pos2 + 1, pos1 - (pos2 + 1)));
        pos2 = controlStr.find(',', pos1 + 1);
        controlData.range.def = stof(controlStr.substr(pos1 + 1, pos2 - (pos1 + 1)));
        pos1 = controlStr.find('}', pos2 + 1);
        controlData.range.step = stof(controlStr.substr(pos2 + 1, pos1 - (pos2 + 1)));
        controls.push_back(controlData);
        //std::cout<< controlData.option <<std::endl;
        controlsPerSensor.erase(0, pos + 1);
      }
      counter++;
      foundBegin = s.find_first_of("[", foundBegin + 1);
    }
    rsRtspClient->m_commandDone = true;
  }
  rsRtspClient->m_cv.notify_one();
}

void RsRTSPClient::continueAfterSETCOMMAND(RTSPClient *rtspClient, int resultCode, char *resultString)
{
  UsageEnvironment &env = rtspClient->envir();               // alias
  RsRTSPClient *rsRtspClient = ((RsRTSPClient *)rtspClient); // alias
  env << "continueAfterSETCOMMAND " << resultCode << " " << resultString << "\n";

  if (NULL!=resultString)
    rsRtspClient->m_lastReturnValue.msg = resultString;
  rsRtspClient->m_lastReturnValue.exit_code = (RsRtspReturnCode)resultCode;
  
  {
    std::lock_guard<std::mutex> lck(rsRtspClient->m_commandMtx);
    rsRtspClient->m_commandDone = true;
  }
  rsRtspClient->m_cv.notify_one();
}

void RsRTSPClient::continueAfterGETCOMMAND(RTSPClient *rtspClient, int resultCode, char *resultString)
{
  UsageEnvironment &env = rtspClient->envir();               // alias
  RsRTSPClient *rsRtspClient = ((RsRTSPClient *)rtspClient); // alias
  float res = std::stof(resultString);
  rsRtspClient->setGetParamResponse(res);

  if (NULL!=resultString)
    rsRtspClient->m_lastReturnValue.msg = resultString;
  rsRtspClient->m_lastReturnValue.exit_code = (RsRtspReturnCode)resultCode;

  {
    std::lock_guard<std::mutex> lck(rsRtspClient->m_commandMtx);
    rsRtspClient->m_commandDone = true;
  }
  rsRtspClient->m_cv.notify_one();
}

// TODO: implementation
void RsRTSPClient::subsessionAfterPlaying(void *clientData)
{
  MediaSubsession *subsession = (MediaSubsession *)clientData;
  RTSPClient *rtspClient = (RTSPClient *)(subsession->miscPtr);
  rtspClient->envir() << "subsessionAfterPlaying\n";
}
void RsRTSPClient::subsessionByeHandler(void *clientData, char const *reason)
{
}
