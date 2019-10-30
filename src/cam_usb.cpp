#include "cam_usb.h"

namespace camera 
{

CamUsb::CamUsb(std::string const& device) : CamInterface(), mCamConfig(NULL),
        mDevice(), mIsOpen(false), mCamInfo(), mMapAttrsCtrlsInt(), mFps(10),
        mBpp(24), mStartTimeGrabbing(), mReceivedFrameCounter(0),
        mpCallbackFunction(NULL), mpPassThroughPointer(NULL) {
    LOG_DEBUG("CamUsb: constructor");
    mDevice = device;
    changeCameraMode(CAM_USB_NONE);
}

CamUsb::~CamUsb() {
    LOG_DEBUG("CamUsb: destructor");
    changeCameraMode(CAM_USB_NONE);
}

void CamUsb::fastInit(int width, int height) {
    std::vector<CamInfo> cam_infos;
    listCameras(cam_infos);
    open(cam_infos[0]);
    const base::samples::frame::frame_size_t size(width, height);
    setFrameSettings(size, base::samples::frame::MODE_JPEG, 3);
}

int CamUsb::listCameras(std::vector<CamInfo> &cam_infos)const {
    LOG_DEBUG("CamUsb: listCameras");

    for(uint32_t i=0; i<cam_infos.size(); ++i) {
        if(cam_infos[i].unique_id == CAM_ID) {
            LOG_INFO("Camera already contained in passed vector, nothing added");
            return 0;
        }
    }

    struct CamInfo cam_info;

    cam_info.unique_id = CAM_ID;
    cam_info.device = mDevice;
    cam_info.interface_type = InterfaceUSB;
    cam_info.reachable = false;
    
    cam_infos.push_back(cam_info);

    return 1; // number of cameras added.
}

bool CamUsb::open(const CamInfo &cam,const AccessMode mode) {
    LOG_DEBUG("CamUsb: open");

    if(mIsOpen) {
        LOG_INFO("Camera %d already opened", cam.unique_id);
        return true;
    }

    changeCameraMode(CAM_USB_V4L2);
    
    mCamInfo = cam; // Assign camera (not allowed in listCameras() as well).
    if(mCamConfig != NULL) {
        mCamInfo.display_name = mCamConfig->getCapabilityCard(); // Add name, not possible in listCameras().
    }

    mIsOpen = true;

    // Will be started in grab().
    return true;
}

bool CamUsb::isOpen()const {
    return mIsOpen;
}

const CamInfo* CamUsb::getCameraInfo()const {
    LOG_DEBUG("CamUsb: getCameraInfo");
    if(mIsOpen) 
        return &mCamInfo;
    else {
        LOG_INFO("Camera not open, no camera info can be returned");
        return NULL;
    }
}

bool CamUsb::close() {
    LOG_DEBUG("CamUsb: close");

    if(mIsOpen) {
        mIsOpen = false;
        changeCameraMode(CAM_USB_NONE);
    } else {
        LOG_INFO("Camera already closed");
    }
    return true;
}

bool CamUsb::grab(const GrabMode mode, const int buffer_len) {
    LOG_DEBUG("CamUsb: grab");
    
    //check if someone tries to change the grab mode during grabbing
    if(act_grab_mode_ != Stop && mode != Stop) {
        if(act_grab_mode_ != mode)
            throw std::runtime_error("Stop grabbing before switching the grab mode!");
        else {
            LOG_INFO("Mode already set to %d, nothing will be changed");
            return true;
        }
    }

    bool image_request_started = false;
    switch(mode) {
        case Stop:
            if(mCamMode == CAM_USB_V4L2) {
                // Cleanup will only be exectued if initRequesting() has be called previously.
                mCamConfig->cleanupRequesting();
            }
            changeCameraMode(CAM_USB_V4L2);
            act_grab_mode_ = mode;
            break;
        case SingleFrame: { // v4l2 image requesting
            changeCameraMode(CAM_USB_V4L2);
            mCamConfig->initRequesting();
            image_request_started = true;
            break;
        }
        case MultiFrame:
        case Continuously: {
            // changeCameraMode(CAM_USB_GST);
            //  // If one of the parameters is 0, the current setting of the camera is used.
            // mCamGst->createDefaultPipeline(true,
            //         image_size_.width, image_size_.height,
            //         (uint32_t)mFps, (uint32_t)mBpp,
            //         image_mode_);
            
            // image_request_started = mCamGst->startPipeline();
            // mReceivedFrameCounter = 0;
            // act_grab_mode_ = mode;
            break;
        }
        default: {
            throw std::runtime_error("The grab mode is not supported by the camera!");
        }
    }
    
    if(image_request_started) {
        gettimeofday(&mStartTimeGrabbing, 0);
    }

    return true;
}                  

bool CamUsb::retrieveFrame(base::samples::frame::Frame &frame,const int timeout) {
    LOG_DEBUG("CamUsb: retrieveFrame");
    
    if(mCamMode == CAM_USB_NONE) {
        LOG_INFO("Frame can not be retrieved, current camera mode is %d", mCamMode);
        return false;
    }
    
    // Buffer will be resized in getBuffer.
    std::vector<uint8_t> buffer_tmp;
    
    // Either v4l2 calls are used to retrieve single images or the gstreamer pipeline.
    // The initialization/cleanup for both methods happens in the grab() function.
    if(mCamMode == CAM_USB_V4L2) {
        try {
            mCamConfig->getBuffer(buffer_tmp, true, timeout);
        } catch(std::runtime_error& e) {
            LOG_ERROR("v4l2: Buffer could not be requested: %s", e.what());
            return false;
        }   
    } else if(mCamMode == CAM_USB_GST) {
        // if(!mCamGst->isPipelineRunning()) {
        //     LOG_WARN("Frame can not be retrieved, because pipeline is not running.");
        //     return false;
        // }
        // bool success = mCamGst->getBuffer(buffer_tmp, true, timeout);
        // if(!success) {
        //     LOG_ERROR("Gstreamer: Buffer could not retrieved.");
        //     return false;
        // }
    }
    
    // TODO In Frame.hpp getChannelCount() returns 1 for UYVY, should be 2?
    int depth = 8;
    if(image_mode_ == base::samples::frame::MODE_UYVY) {
        depth = 16;
    }

    // Only reallocates if required.
    frame.init(image_size_.width, image_size_.height, depth, image_mode_, -1, buffer_tmp.size());
    // Can we directly write to the frame buffer?
    frame.image = buffer_tmp;
    frame.frame_status = base::samples::frame::STATUS_VALID;
    frame.time = base::Time::now();
    
    // Removes the JPEG comment block if required.
    Helpers::removeJpegCommentBlock(frame);

    mReceivedFrameCounter++;
    return true;
}

bool CamUsb::storeFrame(base::samples::frame::Frame& frame, std::string const& file_name) {
    return Helpers::storeImageToFile(frame.image, file_name);
}

bool CamUsb::isFrameAvailable() {
    LOG_DEBUG("CamUsb: isFrameAvailable");

    // if(mCamMode == CAM_USB_GST) {
    //    return mCamGst->hasNewBuffer();
    // } else {
        return true;
        //return mCamConfig->isImageAvailable(2000);
    // }
}

int CamUsb::skipFrames() {
    LOG_DEBUG("CamUsb: skipFrames");

    // if(mCamMode == CAM_USB_GST) {
    //     return mCamGst->skipBuffer() ? 1 : 0;
    // } else if(mCamMode == CAM_USB_V4L2) {
    //     LOG_INFO("Frame skipping is not availabl in V4L2 mode.");
    //     return 1;
    // }
    
    return 1;
}

bool CamUsb::setIpSettings(const CamInfo &cam, const IPSettings &ip_settings)const {
    LOG_DEBUG("CamUsb: setIpSettings");
    throw std::runtime_error("setIpSettings is not yet implemented for the camera interface!");
    return 0;
}

bool CamUsb::setAttrib(const int_attrib::CamAttrib attrib, const int value) {
    LOG_DEBUG("CamUsb: setAttrib int");
    
    if(mCamMode != CAM_USB_V4L2) {
        LOG_INFO("An int attribute can not be set, current mode is %d",mCamMode);
        return false;
    }

    std::map<int_attrib::CamAttrib, int>::iterator it = mMapAttrsCtrlsInt.find(attrib);
    if(it == mMapAttrsCtrlsInt.end())
        throw std::runtime_error("Unknown attribute!");
    else {
        try {
            mCamConfig->writeControlValue(it->second, value);
        } catch(std::runtime_error& e) {
            LOG_ERROR("Set integer attribute %d to %d: %s", attrib, value, e.what());
        }
    }
    return true;
}


bool CamUsb::setAttrib(const double_attrib::CamAttrib attrib, const double value) {
    LOG_DEBUG("CamUsb: seAttrib double");
   
    if(mCamMode != CAM_USB_V4L2) {
        LOG_INFO("A double attribute. can not be set, current mode is %d",mCamMode);
        return false;
    }

    switch(attrib) {

        case double_attrib::FrameRate:
        case double_attrib::StatFrameRate: {
            mCamConfig->writeFPS((uint32_t)value);
            float cam_fps_tmp = 0;
            mCamConfig->readFPS(&cam_fps_tmp);
            if(1/(float)value != cam_fps_tmp) {
                LOG_WARN("Written (%4.2f) and read (%4.2f) FPS differ, read value will be used", 
                         1/(float)value, cam_fps_tmp);
            }
            mFps = cam_fps_tmp;
            break;
        }
        default:
            throw std::runtime_error("Unknown attribute!");
    }
    return true;
}

bool setAttrib(const str_attrib::CamAttrib attrib,const std::string value) {
    LOG_DEBUG("CamUsb: setAttrib string");
    throw std::runtime_error("setAttrib str_attrib is not yet implemented for the camera interface!");
    return 0;
}

bool CamUsb::setAttrib(const enum_attrib::CamAttrib attrib) {
    LOG_DEBUG("CamUsb: setAttrib enum %i", attrib);

    if(mCamMode != CAM_USB_V4L2) {
        LOG_INFO("Stop image requesting before setting an enum attribute.");
        return false;
    }

    switch(attrib) {
        case enum_attrib::WhitebalModeToManual: {
            mCamConfig->writeControlValue(V4L2_CID_AUTO_WHITE_BALANCE, 0);
            break;
        }
        case enum_attrib::WhitebalModeToAuto: {
            mCamConfig->writeControlValue(V4L2_CID_AUTO_WHITE_BALANCE, 1);
            break;
        }
        case enum_attrib::GainModeToManual: {
            mCamConfig->writeControlValue(V4L2_CID_AUTOGAIN, 0);
            break;
        }
        case enum_attrib::GainModeToAuto: {
            mCamConfig->writeControlValue(V4L2_CID_AUTOGAIN, 1);
            break;
        }
        case enum_attrib::PowerLineFrequencyDisabled: {
            mCamConfig->writeControlValue(V4L2_CID_POWER_LINE_FREQUENCY, V4L2_CID_POWER_LINE_FREQUENCY_DISABLED);
            break;
        }
        case enum_attrib::PowerLineFrequencyTo50: {
            mCamConfig->writeControlValue(V4L2_CID_POWER_LINE_FREQUENCY, V4L2_CID_POWER_LINE_FREQUENCY_50HZ);
            break;
        }
        case enum_attrib::PowerLineFrequencyTo60: {
            mCamConfig->writeControlValue(V4L2_CID_POWER_LINE_FREQUENCY, V4L2_CID_POWER_LINE_FREQUENCY_60HZ);
            break;
        }
        case enum_attrib::ExposureModeToAuto: {
            uint32_t id = V4L2_EXPOSURE_AUTO;
            if(!mCamConfig->isControlIdValid(id)) {
                id = V4L2_CID_EXPOSURE_AUTO_PRIORITY;
            }
            mCamConfig->writeControlValue(id, V4L2_EXPOSURE_AUTO);
            break;
        }
        case enum_attrib::ExposureModeToManual: {
            uint32_t id = V4L2_EXPOSURE_AUTO;
            if(!mCamConfig->isControlIdValid(id)) {
                id = V4L2_CID_EXPOSURE_AUTO_PRIORITY;
            }
            mCamConfig->writeControlValue(id, V4L2_EXPOSURE_MANUAL);
            break;
        }
        // attribute unknown or not supported (yet)
        default:
            throw std::runtime_error("Unknown attribute!");
    }

    return true;
}

bool CamUsb::isAttribAvail(const int_attrib::CamAttrib attrib) {
    LOG_DEBUG("CamUsb: isAttribAvail int");

    if(mCamMode != CAM_USB_V4L2) {
        LOG_INFO("Stop image requesting before checking whether an int attribute is available.");
        return false;
    }

    std::map<int_attrib::CamAttrib, int>::iterator it = mMapAttrsCtrlsInt.find(attrib);
    if(it == mMapAttrsCtrlsInt.end())
        return false;
    else
        return mCamConfig->isControlIdWritable(it->second);
}

bool CamUsb::isAttribAvail(const double_attrib::CamAttrib attrib) {
    LOG_DEBUG("CamUsb: isAttribAvail double");
    
    if(mCamMode != CAM_USB_V4L2) {
        if(attrib == double_attrib::FrameRate || attrib == double_attrib::StatFrameRate) { 
            return true;
        } else {
            LOG_INFO("Stop image requesting before checking whether an double attribute is available.");
            return false;
        }
    }

    switch(attrib) {

        case double_attrib::FrameRate:
        case double_attrib::StatFrameRate: {
            bool ret = mCamConfig->hasCapabilityStreamparm(V4L2_CAP_TIMEPERFRAME);
            return ret;
        }
        default:
            return false;
    }
    return false;
}
        
bool CamUsb::isAttribAvail(const enum_attrib::CamAttrib attrib) {
    LOG_DEBUG("CamUsb:isAttriAvail enum");
    
    if(mCamMode != CAM_USB_V4L2) {
        LOG_INFO("Stop image requesting before checking whether an enum attribute is available.");
        return false;
    }

    switch(attrib) {
        case enum_attrib::WhitebalModeToManual:
        case enum_attrib::WhitebalModeToAuto: {
            return mCamConfig->isControlIdValid(V4L2_CID_AUTO_WHITE_BALANCE);
        }
        case enum_attrib::GainModeToManual:
        case enum_attrib::GainModeToAuto: {
            return mCamConfig->isControlIdValid(V4L2_CID_AUTOGAIN);
        }
        case enum_attrib::PowerLineFrequencyDisabled:
        case enum_attrib::PowerLineFrequencyTo50:
        case enum_attrib::PowerLineFrequencyTo60: {
            return mCamConfig->isControlIdValid(V4L2_CID_POWER_LINE_FREQUENCY);
        }
        case enum_attrib::ExposureModeToAuto:
        case enum_attrib::ExposureModeToManual: {
            return mCamConfig->isControlIdValid(V4L2_CID_EXPOSURE_AUTO_PRIORITY) || 
                mCamConfig->isControlIdValid(V4L2_EXPOSURE_AUTO);
        }
        // attribute unknown or not supported (yet)
        default:
            return false;
    }
    return false;
}

int CamUsb::getAttrib(const int_attrib::CamAttrib attrib) {
    LOG_DEBUG("CamUsb: getAttrib int");

    if(mCamMode != CAM_USB_V4L2) {
        throw std::runtime_error("Stop image requesting before getting an int attribute.");
    }

    std::map<int_attrib::CamAttrib, int>::iterator it = mMapAttrsCtrlsInt.find(attrib);
    if(it == mMapAttrsCtrlsInt.end())
        throw std::runtime_error("Unknown attribute!");

    int32_t value = 0;
    mCamConfig->getControlValue(it->second, &value);    

    return value; 
}

double CamUsb::getAttrib(const double_attrib::CamAttrib attrib) {
    LOG_DEBUG("CamUsb: getAttrib double");

    if(mCamMode != CAM_USB_V4L2) {
        // If FrameRate or StatFrameRate is requested, the current fps are returned if the pipeline is running,
        // otherwise the fps which is set on the camera. 
        if(attrib == double_attrib::FrameRate || attrib == double_attrib::StatFrameRate) {
            return calculateFPS();
        }
        throw std::runtime_error("Stop image requesting before getting a double attribute.");
    }

    switch(attrib) {
        case double_attrib::FrameRate:
        case double_attrib::StatFrameRate: {
            // Just returns the stored framerate, which has been requested in setAttrib().
            mCamConfig->readFPS(&mFps);
            return mFps;
        }
        default:
            throw std::runtime_error("Unknown attribute!");
    }
}

bool CamUsb::isAttribSet(const enum_attrib::CamAttrib attrib) {
    LOG_DEBUG("CamUsb: isAttribSet enum");
   
    if(mCamMode != CAM_USB_V4L2) {
        throw std::runtime_error("Stop image requesting before check whether a enum attribute is set.");
        return false;
    }

    int32_t value = 0;
    switch(attrib) {
        case enum_attrib::WhitebalModeToManual: 
            mCamConfig->getControlValue(V4L2_CID_AUTO_WHITE_BALANCE, &value);
            return value == 0;
        case enum_attrib::WhitebalModeToAuto:
            mCamConfig->getControlValue(V4L2_CID_AUTO_WHITE_BALANCE, &value);
            return value == 1;
        case enum_attrib::GainModeToManual:
            mCamConfig->getControlValue(V4L2_CID_AUTOGAIN, &value);
            return value == 0;
        case enum_attrib::GainModeToAuto:
            mCamConfig->getControlValue(V4L2_CID_AUTOGAIN, &value);
            return value == 1;
        case enum_attrib::PowerLineFrequencyDisabled:
            mCamConfig->getControlValue(V4L2_CID_POWER_LINE_FREQUENCY, &value); 
            return value == 0;
        case enum_attrib::PowerLineFrequencyTo50:
            mCamConfig->getControlValue(V4L2_CID_POWER_LINE_FREQUENCY, &value); 
            return value == 1;
        case enum_attrib::PowerLineFrequencyTo60:
             mCamConfig->getControlValue(V4L2_CID_POWER_LINE_FREQUENCY, &value); 
            return value == 2;
        case enum_attrib::ExposureModeToAuto:
            if(!mCamConfig->getControlValue(V4L2_CID_EXPOSURE_AUTO_PRIORITY, &value)) {
                mCamConfig->getControlValue(V4L2_EXPOSURE_AUTO, &value);
            }
            return value == V4L2_EXPOSURE_AUTO;
        case enum_attrib::ExposureModeToManual:
             if(!mCamConfig->getControlValue(V4L2_CID_EXPOSURE_AUTO_PRIORITY, &value)) {
                mCamConfig->getControlValue(V4L2_EXPOSURE_AUTO, &value);
            }
            return value == V4L2_EXPOSURE_MANUAL;
        // attribute unknown or not supported (yet)
        default:
            throw std::runtime_error("Unknown attribute");
    }
}

bool CamUsb::isV4L2AttribAvail(const int control_id, std::string name) {
    LOG_DEBUG("CamUsb: isV4L2AttribAvail");
   
    if(mCamMode != CAM_USB_V4L2) {
        LOG_INFO("Stop image requesting before check whether a v4l2 control attribute is available.");
        return false;
    }

    if(!mCamConfig->isControlIdValid(control_id)) {
        return false;
    }

    if(!name.empty()) {
        std::string control_name;
        mCamConfig->getControlName(control_id, &control_name);
        if(control_name.compare(name) != 0) { // control names differ
            LOG_DEBUG("Control names differ. Passed name: %s, control name: %s", 
                    name.c_str(), control_name.c_str());
            return false;
        }
    }

    return true;
}

int CamUsb::getV4L2Attrib(const int control_id) {
    LOG_DEBUG("CamUsb: getV4L2Attrib");
   
    if(mCamMode != CAM_USB_V4L2) {
        throw std::runtime_error("Stop image requesting before getting a v4l2 attribute.");
    }

    int value_tmp = 0;

    if(!mCamConfig->getControlValue(control_id, &value_tmp)) {
         throw std::runtime_error("Unkown attribute");
    }

    return value_tmp;
}

bool CamUsb::setV4L2Attrib(const int control_id, const int value) {
    LOG_DEBUG("CamUsb: setV4L2Attrib");
   
    if(mCamMode != CAM_USB_V4L2) {
        throw std::runtime_error("Stop image requesting before setting a v4l2 attribute.");
    }
 
    mCamConfig->writeControlValue(control_id, value);
    return true;
}

bool CamUsb::setFrameSettings(  const base::samples::frame::frame_size_t size,
                                      const base::samples::frame::frame_mode_t mode,
                                      const uint8_t color_depth,
                                      const bool resize_frames) {

    LOG_DEBUG("CamUsb: setFrameSettings");
    
    if(mCamMode != CAM_USB_V4L2) {
        LOG_INFO("Stop the device before setting frame settings.");
        return false;
    }

    LOG_DEBUG("color_depth is set to %d", (int)color_depth);

    // Hack: If RGB is requested and not available on the camera, YUYV will be 
    // used and internally converted to RGB.
    uint32_t v4l2_image_format = mCamConfig->toV4L2ImageFormat(mode);
    if(v4l2_image_format == 0) {
        LOG_INFO("Frame mode not available on the camera, using default camera mode.");
        LOG_INFO("v4l2 image requesting will probably support an unexpeted format");
        mCamConfig->writeImagePixelFormat(size.width, size.height);
    } else {
        mCamConfig->writeImagePixelFormat(size.width, size.height, v4l2_image_format); // use V4L2_PIX_FMT_YUV420?
    }
    uint32_t width = 0, height = 0;
    mCamConfig->getImageWidth(&width);
    mCamConfig->getImageHeight(&height);

    base::samples::frame::frame_size_t size_tmp;
    size_tmp.width = (uint16_t)width;
    size_tmp.height = (uint16_t)height;

    image_size_ = size_tmp;
    image_mode_ = mode;
    image_color_depth_ = color_depth;
    return true;
}

bool CamUsb::getFrameSettings(base::samples::frame::frame_size_t &size,
                                base::samples::frame::frame_mode_t &mode,
                                uint8_t &color_depth) {

    LOG_DEBUG("CamUsb: getFrameSettings");

    size = image_size_;
    mode = image_mode_;
    color_depth = image_color_depth_;
    
    return true;
}

bool CamUsb::triggerFrame() {
    return true;
}

bool CamUsb::setToDefault() {
    LOG_DEBUG("CamUsb: setToDefault");
    
    if(mCamMode != CAM_USB_V4L2) {
        LOG_INFO("Stop image requesting before set camera parameters to default.");
        return false;
    }

    mCamConfig->setControlValuesToDefault();
    return true;
}

bool CamUsb::printCameraInformations() {
    if(mCamMode != CAM_USB_V4L2) {
        LOG_INFO("Stop image requesting before set camera parameters to default.");
        return false;
    }
    printf("\nCAMERA INFORMATIONS\n");
    mCamConfig->listCapabilities();
    mCamConfig->listControls();
    mCamConfig->listImageFormat();
    mCamConfig->listStreamparm();
    return true;
}

void CamUsb::getRange(const int_attrib::CamAttrib attrib,int &imin,int &imax) {
    LOG_DEBUG("CamUsb: getRange");
    
    if(mCamMode != CAM_USB_V4L2) {
        LOG_INFO("Stop image requesting before requesting range.");
        return;
    }

    std::map<int_attrib::CamAttrib, int>::iterator it = mMapAttrsCtrlsInt.find(attrib);
    if(it != mMapAttrsCtrlsInt.end()) {
        int32_t min = 0, max = 0;
        mCamConfig->getControlMinimum(it->second, &min);
        imin = min;
        mCamConfig->getControlMaximum(it->second, &max);
        imax = max;
    }
}

int CamUsb::getFileDescriptor() const {
    LOG_DEBUG("CamUsb: getFileDescriptor");

    if(mCamMode != CAM_USB_GST) {
        LOG_INFO("Start pipeline to request the corresponding file descriptor");
        return -1;
    }
   
    // int fd = mCamGst->getFileDescriptor();
    // if(fd == -1) {
    //     LOG_INFO("File descriptor could not be requested, start pipeline with grab() first");
    // }
    // return fd;
    return -1;
}

void CamUsb::createAttrsCtrlMaps(CamConfig* cam_config) {
    LOG_DEBUG("CamUsb: createAttrsCtrlMaps");
    
    typedef std::pair<int_attrib::CamAttrib, int> ac_int;
    mMapAttrsCtrlsInt.clear();
    mMapAttrsCtrlsInt.insert(ac_int(int_attrib::BrightnessValue,V4L2_CID_BRIGHTNESS));
    mMapAttrsCtrlsInt.insert(ac_int(int_attrib::ContrastValue,V4L2_CID_CONTRAST));
    mMapAttrsCtrlsInt.insert(ac_int(int_attrib::SaturationValue,V4L2_CID_SATURATION));
    mMapAttrsCtrlsInt.insert(ac_int(int_attrib::WhitebalValue,V4L2_CID_WHITE_BALANCE_TEMPERATURE));
    mMapAttrsCtrlsInt.insert(ac_int(int_attrib::SharpnessValue,V4L2_CID_SHARPNESS));
    mMapAttrsCtrlsInt.insert(ac_int(int_attrib::BacklightCompensation,V4L2_CID_BACKLIGHT_COMPENSATION));
    uint32_t valid_exposure_id = V4L2_CID_EXPOSURE_ABSOLUTE;
    if(!cam_config->isControlIdValid(valid_exposure_id)) {
        valid_exposure_id = V4L2_CID_EXPOSURE;
    }
    mMapAttrsCtrlsInt.insert(ac_int(int_attrib::ExposureValue,valid_exposure_id));
}

void CamUsb::changeCameraMode(enum CAM_USB_MODE cam_usb_mode) {

    LOG_DEBUG("Will change camera mode to: %s", camera::ModeTxt[cam_usb_mode].c_str());

    if(cam_usb_mode == mCamMode) {
        LOG_DEBUG("cam-mode %d already set, nothing changed.",cam_usb_mode);
        return;
    }

    // if(mCamGst != NULL) {
    //     delete mCamGst;
    //     mCamGst = NULL;
    // }

    if(mCamConfig != NULL) {
        delete mCamConfig;
        mCamConfig = NULL;
    }

    switch (cam_usb_mode) {
        case CAM_USB_NONE:
            LOG_INFO("Camera configuration mode set to none");
            mCamMode = CAM_USB_NONE;
            break;
        case CAM_USB_V4L2:
            LOG_INFO("Camera configuration mode via v4l2 activated");
            mCamConfig = new CamConfig(mDevice);
            mCamMode = CAM_USB_V4L2;
            createAttrsCtrlMaps(mCamConfig);
            break;
        case CAM_USB_GST:
            // LOG_INFO("Camera image transfer mode via gst activated");
            // mCamGst = new CamGst(mDevice);
            // mCamMode = CAM_USB_GST;
            break;
        default:
            LOG_WARN("Unknown cam-mode %d passed, modus will be set to CAM_USB_NONE");
            mCamMode = CAM_USB_NONE;
            break;
    }
}

} // end namespace camera
