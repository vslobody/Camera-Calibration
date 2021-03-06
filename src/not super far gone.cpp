/*
Kyle Meredith 2017
Middlebury College undergraduate summer research with Daniel Scharstein

This program is adapted from calibration.cpp, camera_calibration.cpp, and stereo_calib.cpp,
which are example calibration programs provided by opencv. It supports unique
functionality with Rafael Munoz-Salinas' ArUco library, including calibration
with a 3D ArUco box rig.

The program has three modes: intrinsic calibration, stereo calibration, and live
feed preview. It supports three patterns: chessboard, aruco single, and aruco box.

Read the read me for more information and guidance.
*/

#include "opencv2/core/core.hpp"
#include "opencv2/imgproc/imgproc.hpp"
#include "opencv2/calib3d/calib3d.hpp"
#include "opencv2/highgui/highgui.hpp"
#include <aruco.h>

#include <iostream>
#include <fstream>
#include <sstream>
#include <cctype>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <dirent.h>

using namespace cv;
using namespace aruco;
using namespace std;

const char* previewHelp =
    "Preview functions:\n"
        "  <ESC>, 'q' - quit the program\n"
        "  'u' - toggle undistortion on/off\n"
        "  'c' - toggle ArUco marker coordinates/IDs\n";

//struct to store parameters for intrinsic calibration
struct intrinsicCalibration {
    Mat cameraMatrix, distCoeffs;   //intrinsic camera matrices
    vector<Mat> rvecs, tvecs;       //extrinsic rotation and translation vectors for each image
    vector<vector<Point2f> > imagePoints;   //corner points on 2d image
    vector<vector<Point3f> > objectPoints;  //corresponding 3d object points
    vector<float> reprojErrs;   //vector of reprojection errors for each pixel
    double totalAvgErr = 0;     //average error across every pixel
};

//struct to store parameters for stereo calibration
struct stereoCalibration {
    Mat R, T, E, F;         //Extrinsic matrices (rotation, translation, essential, fundamental)
    Mat R1, R2, P1, P2, Q;  //Rectification parameters (rectification transformations, projection matrices, disparity-to-depth mapping matrix)
    Rect validRoi[2];       //Rectangle within the rectified image that contains all valid points
};

struct arucoPattern {
    vector <MarkerMap> configList;  // Aruco marker map configs
    vector <string> planeList;      // Corresponding 3D planes for each marker map config
    Point2f offset;                 // The x y transformation to make the origin the bottom left corner
    int denominator;                // The denominator to divide all point values to become integers
}

class Settings
{
public:
    Settings() : goodInput(false) {}
    enum Pattern { CHESSBOARD, ARUCO_SINGLE, ARUCO_BOX, NOT_EXISTING };
    enum Mode { INTRINSIC, STEREO, PREVIEW, INVALID };

    void write(FileStorage& fs) const       //Write serialization for this class
    {
        fs << "{" << "Mode" << modeInput
                  << "Calibration_Pattern" <<  patternInput

                  << "ChessboardSize_Width"  <<  boardSize.width
                  << "ChessboardSize_Height" <<  boardSize.height
                  << "SquareSize" << squareSize

                  << "imageList_Filename" <<  imageListFilename
                  << "arucoConfigList_Filename" <<  configListFilename
                  << "intrinsicInput_Filename" <<  intrinsicInputFilename

                  << "IntrinsicOutput_Filename" <<  intrinsicOutput
                  << "ExtrinsicOutput_Filename" <<  extrinsicOutput

                  << "UndistortedImages_Path" <<  undistortedPath
                  << "RectifiedImages_Path" <<  rectifiedPath
                  << "DetectedImages_Path" <<  detectedPath

                  << "Calibrate_FixDistCoeffs" << fixDistCoeffs
                  << "Calibrate_FixAspectRatio" <<  aspectRatio
                  << "Calibrate_AssumeZeroTangentialDistortion" <<  assumeZeroTangentDist
                  << "Calibrate_FixPrincipalPointAtTheCenter" <<  fixPrincipalPoint

                  << "Show_UndistortedImages" <<  showUndistorted
                  << "Show_RectifiedImages" <<  showRectified
                  << "Show_ArucoMarkerCoordinates" << showArucoCoords
                  << "Wait_NextDetectedImage" << wait

                  << "LivePreviewCameraID" <<  cameraIDInput
           << "}";
    }
    void read(const FileNode& node)                          //Read serialization for this class
    {
        node["Mode"] >> modeInput;
        node["Calibration_Pattern"] >> patternInput;

        node["ChessboardSize_Width" ] >> boardSize.width;
        node["ChessboardSize_Height"] >> boardSize.height;
        node["SquareSize"]  >> squareSize;

        node["imageList_Filename"] >> imageListFilename;
        node["arucoConfigList_Filename"] >> configListFilename;
        node["intrinsicInput_Filename"] >> intrinsicInputFilename;

        node["IntrinsicOutput_Filename"] >> intrinsicOutput;
        node["ExtrinsicOutput_Filename"] >> extrinsicOutput;

        node["UndistortedImages_Path"] >> undistortedPath;
        node["RectifiedImages_Path"] >> rectifiedPath;
        node["DetectedImages_Path"] >> detectedPath;

        node["Calibrate_FixDistCoeffs"] >> fixDistCoeffs;
        node["Calibrate_FixAspectRatio"] >> aspectRatio;
        node["Calibrate_AssumeZeroTangentialDistortion"] >> assumeZeroTangentDist;
        node["Calibrate_FixPrincipalPointAtTheCenter"] >> fixPrincipalPoint;

        node["Show_UndistortedImages"] >> showUndistorted;
        node["Show_RectifiedImages"] >> showRectified;
        node["Show_ArucoMarkerCoordinates"] >> showArucoCoords;
        node["Wait_NextDetectedImage"] >> wait;

        node["LivePreviewCameraID"] >> cameraIDInput;
        interprate();
    }
    void interprate()
    {
        goodInput = true;

        mode = INVALID;
        if (!modeInput.compare("INTRINSIC")) mode = INTRINSIC;
        if (!modeInput.compare("STEREO")) mode = STEREO;
        if (!modeInput.compare("PREVIEW")) mode = PREVIEW;
        if (mode == INVALID)
            {
                cerr << "Invalid calibration mode: " << modeInput << endl;
                goodInput = false;
            }

        calibrationPattern = NOT_EXISTING;
        if (!patternInput.compare("CHESSBOARD")) calibrationPattern = CHESSBOARD;
        if (!patternInput.compare("ARUCO_SINGLE")) calibrationPattern = ARUCO_SINGLE;
        if (!patternInput.compare("ARUCO_BOX")) calibrationPattern = ARUCO_BOX;
        if (calibrationPattern == NOT_EXISTING)
            {
                cerr << "Invalid calibration pattern: " << patternInput << endl;
                goodInput = false;
            }

        if (boardSize.width <= 0 || boardSize.height <= 0)
        {
            cerr << "Invalid chessboard size: " << boardSize.width << " " << boardSize.height << endl;
            goodInput = false;
        }
        if (squareSize <= 10e-6)
        {
            cerr << "Invalid square size " << squareSize << endl;
            goodInput = false;
        }

        if (mode == PREVIEW)
        {
            if (cameraIDInput[0] >= '0' && cameraIDInput[0] <= '9')
            {
                stringstream ss(cameraIDInput);
                ss >> cameraID;
                capture.open(cameraID);
            }
            if (!capture.isOpened())
            {
                cerr << "Invalid camera ID for live preview: " << cameraIDInput << endl;
                goodInput = false;
            }
            else
                printf( "\n%s", previewHelp );
        }
        else if (readImageList(imageListFilename))
        {
            nImages = (int)imageList.size();
            if (mode == STEREO)
                if (nImages % 2 != 0) {
                    cerr << "Image list must have even # of elements for stereo calibration" << endl;
                    goodInput = false;
                }
        }
        else {
            cerr << "Invalid image list: " << imageListFilename << endl;
            goodInput = false;
        }

        if (calibrationPattern != CHESSBOARD)       //Aruco pattern
        {
            if (readConfigList(configListFilename)) {
                nConfigs = (int)configList.size();
                if (calibrationPattern == ARUCO_SINGLE && nConfigs != 1)
                {
                    cerr << "Incorrect # of configs for single aruco pattern: " << nConfigs << endl;
                    goodInput = false;
                }
                else if (calibrationPattern == ARUCO_BOX && nConfigs != 3)
                {
                    cerr << "Incorrect # of configs for aruco box rig: " << nConfigs << endl;
                    goodInput = false;
                }
            }
            else {
                cerr << "Invalid aruco config list: " << configListFilename << endl;
                goodInput = false;
            }
        }

        useIntrinsicInput = false;
        if (readIntrinsicInput(intrinsicInputFilename)) {
            useIntrinsicInput = true;
        }
        else if (calibrationPattern == ARUCO_BOX) {
            cerr << "Must input intrinsics to calibrate with ARUCO_BOX pattern" << endl;
            goodInput = false;
        }

        flag = 0;
        int digit, shift;
        for (int i=0; i<5; i++)
        {
            digit = fixDistCoeffs[i] - '0';     //gets first digit as int
            if (i >= 3)
                shift = i + 3;
            else
                shift = i;
            if (digit) {
                flag |= CV_CALIB_FIX_K1 << shift;
            }
        }

        if(fixPrincipalPoint)       flag |= CV_CALIB_FIX_PRINCIPAL_POINT;
        if(assumeZeroTangentDist)   flag |= CV_CALIB_ZERO_TANGENT_DIST;
        if(aspectRatio)             flag |= CV_CALIB_FIX_ASPECT_RATIO;
    }

    Mat imageSetup(int imageIndex)
    {
        Mat img;
        if( capture.isOpened() )
        {
            Mat capImg;
            capture >> capImg;
            capImg.copyTo(img);
        }
        else if( imageIndex < (int)imageList.size() )
            img = imread(imageList[imageIndex], CV_LOAD_IMAGE_COLOR);

        // if the image is too big, resize it
        if (img.cols>1280) resize(img, img, Size(), 0.5, 0.5);

        return img;
    }

    bool readImageList( const string& filename )
    {
        imageList.clear();
        FileStorage fs(filename, FileStorage::READ);
        if( !fs.isOpened() )
            return false;
        FileNode n = fs.getFirstTopLevelNode();
        if( n.type() != FileNode::SEQ )
            return false;
        FileNodeIterator it = n.begin(), it_end = n.end();
        for( ; it != it_end; ++it )
            imageList.push_back((string)*it);
        return true;
    }

    bool readConfigList( const string& filename )
    {
        configList.resize(0);
        FileStorage fs(filename, FileStorage::READ);
        if( !fs.isOpened() )
            return false;

        fs["Marker_Size"] >> squareSize;

        FileNode n = fs["Configs"];
        FileNodeIterator it = n.begin(), it_end = n.end();
        for( ; it != it_end; ++it ) {
            MarkerMap config;
            config.readFromFile((string)*it);
            configList.push_back(config);
        }

        n = fs["Planes"];
        it = n.begin(), it_end = n.end();
        for( ; it != it_end; ++it ) {
            planeList.push_back((string)*it);
        }
        return true;
    }

    bool readIntrinsicInput( const string& filename )
    {
        FileStorage fs(filename, FileStorage::READ);
        if( !fs.isOpened() ) {
            if ( filename == "0" )       // Intentional lack of input
                return false;
            else {                              // Unintentional invalid input
                cerr << "Invalid intrinsic input: " << filename << endl;
                return false;
            }
        }
        fs["camera_matrix"] >> intrinsicInput.cameraMatrix;
        fs["distortion_coefficients"] >> intrinsicInput.distCoeffs;
        return true;
    }

    void saveIntrinsics(intrinsicCalibration &inCal)
    {
        if (intrinsicOutput == "0")
            return;
        FileStorage fs( intrinsicOutput, FileStorage::WRITE );

        time_t tm;
        time( &tm );
        struct tm *t2 = localtime( &tm );
        char buf[1024];
        strftime( buf, sizeof(buf)-1, "%c", t2 );
        fs << "calibration_Time" << buf;

        fs << "image_width" << imageSize.width;
        fs << "image_height" << imageSize.height;

        fs << "calibration_pattern" << patternInput;
        if (calibrationPattern == CHESSBOARD)
        {
            fs << "board_width" << boardSize.width;
            fs << "board_height" << boardSize.height;
            fs << "square_size" << squareSize;
        }

        if( flag & CV_CALIB_FIX_ASPECT_RATIO )
            fs << "aspectRatio" << aspectRatio;

        if( flag )
        {
            sprintf( buf, "%s%s%s%s%s%s%s%s%s",
                flag & CV_CALIB_FIX_K1 ? "+fix_k1 " : "",
                flag & CV_CALIB_FIX_K2 ? "+fix_k2 " : "",
                flag & CV_CALIB_FIX_K3 ? "+fix_k3 " : "",
                flag & CV_CALIB_FIX_K4 ? "+fix_k4 " : "",
                flag & CV_CALIB_FIX_K5 ? "+fix_k5 " : "",
                flag & CV_CALIB_USE_INTRINSIC_GUESS ? "+use_intrinsic_guess " : "",
                flag & CV_CALIB_FIX_ASPECT_RATIO ? "+fix_aspectRatio " : "",
                flag & CV_CALIB_FIX_PRINCIPAL_POINT ? "+fix_principal_point " : "",
                flag & CV_CALIB_ZERO_TANGENT_DIST ? "+zero_tangent_dist " : "" );
            // cvWriteComment( *fs, buf, 0 );
        }
        fs << "calibration_flags" << buf;
        fs << "flagValue" << flag;

        fs << "camera_matrix" << inCal.cameraMatrix;
        fs << "distortion_coefficients" << inCal.distCoeffs;

        fs << "avg_reprojection_error" << inCal.totalAvgErr;
        if( !inCal.reprojErrs.empty() )
            fs << "per_view_reprojection_errors" << Mat(inCal.reprojErrs);
    }

    void saveExtrinsics(stereoCalibration &sterCal)
    {
        if (extrinsicOutput == "0")
            return;
        FileStorage fs( extrinsicOutput, FileStorage::WRITE );
        time_t tm;
        time( &tm );
        struct tm *t2 = localtime( &tm );
        char buf[1024];
        strftime( buf, sizeof(buf)-1, "%c", t2 );
        fs << "calibration_Time" << buf;

        fs << "calibration_pattern" << patternInput;

        fs << "Stereo_Parameters";
        fs << "{" << "Rotation_Matrix"     << sterCal.R
                  << "Translation_Vector"  << sterCal.T
                  << "Essential_Matrix"    << sterCal.E
                  << "Fundamental_Matrix"  << sterCal.F
           << "}";

        fs << "Rectification_Parameters";
        fs << "{" << "Rectification_Transformation_1"       << sterCal.R1
                  << "Rectification_Transformation_2"       << sterCal.R2
                  << "Projection_Matrix_1"                  << sterCal.P1
                  << "Projection_Matrix_2"                  << sterCal.P2
                  << "Disparity-to-depth_Mapping_Matrix"    << sterCal.Q
           << "}";
    }

public:
//--------------------------Calibration configuration-------------------------//
    // Program modes:
    //    INTRINSIC  — calculates intrinsics parameters and  undistorts images
    //    STEREO     — calculates extrinsic stereo paramaters and rectifies images
    //    PREVIEW    — detects pattern on live feed, previewing detection and undistortion
    Mode mode;
    Pattern calibrationPattern;   // Three supported calibration patterns: CHESSBOARD, ARUCO_SINGLE, ARUCO_BOX

    Size boardSize;     // Size of chessboard (number of inner corners per chessboard row and column)
    float squareSize;   // The size of a square in some user defined metric system (pixel, millimeter, etc.)

//-----------------------------Input settings---------------------------------//
    vector<string> imageList;   // Image list to run calibration
    string imageListFilename;   // Input filename for image list

    vector <MarkerMap> configList;  // Aruco marker map configs
    string configListFilename;      // Input filename for aruco config files

    //Intrinsic input can be used as an initial estimate for intrinsic calibration,
    //as fixed intrinsics for stereo calibration, or to preview undistortion in preview mode
    //Leave filename at "0" to calculate new intrinsics
    intrinsicCalibration intrinsicInput; // Struct to store inputted intrinsics
    string intrinsicInputFilename;       // Intrinsic input filename
    bool useIntrinsicInput;              // Boolean to simplify program

//-----------------------------Output settings--------------------------------//
    string intrinsicOutput;    // File to write results of intrinsic calibration
    string extrinsicOutput;    // File to write extrisics of stereo calibration

    // LEAVE THESE SETTINGS AT "0" TO NOT SAVE IMAGES
    string undistortedPath;    // Path at which to save undistorted images
    string rectifiedPath;      // Path at which to save rectified images
    string detectedPath;       // Path at which to save images with detected patterns

//-----------------------Intrinsic Calibration settings-----------------------//
    // It is recommended to fix distortion coefficients 3-5 ("00111"). Only 1-2 are needed
    // in most cases, and 3 produces significant distortion in stereo rectification
    string fixDistCoeffs;         // A string of five digits (0 or 1) that control which distortion coefficients will be fixed (1 = fixed)
    float aspectRatio;            // The aspect ratio. If it is non zero, it will be fixed in calibration
    bool assumeZeroTangentDist;   // Assume zero tangential distortion
    bool fixPrincipalPoint;       // Fix the principal point at the center
    int flag;                     // Flag to modify calibration

//--------------------------------UI settings---------------------------------//
    bool showUndistorted;   // Show undistorted images after intrinsic calibration
    bool showRectified;     // Show rectified images after stereo calibration
    bool showArucoCoords;   // Draw each marker with its 3D coordinate. If false, IDs will be printed
    bool wait;              // Wait until a key is pressed to show the next detected image

//-----------------------------Program variables------------------------------//
    int nImages;        // Number of images in the image list
    Size imageSize;     // Size of each image
    int nConfigs;       // Number of config files in config list

//---------------------------Live Preview settings----------------------------//
    int cameraID;           //ID for live preview camera. Generally "0" is built in webcam
    VideoCapture capture;   //Live capture object

    bool goodInput;         //Tracks input validity
private:
    // Input variables only needed to set up settings
    string modeInput;
    string patternInput;
    string cameraIDInput;
};

static void read(const FileNode& node, Settings& x, const Settings& default_value = Settings())
{
    if(node.empty())
        x = default_value;
    else
        x.read(node);
}

// Uncomment write() if you want to save your settings, using code like this:
//
// FileStorage fs("settingsOutput.yml", FileStorage::WRITE);
// fs << "Settings" << s;

// static void write(FileStorage& fs, const string&, const Settings& x)
// {
//     x.write(fs);
// }

const char* liveCaptureHelp =
    "When the live video from camera is used as input, the following hot-keys may be used:\n"
        "  <ESC>, 'q' - quit the program\n"
        "  'u' - switch undistortion on/off\n";

//-----------------------Debugging helper functions---------------------------//
void printMat(Mat m, const char *name)
{
    Size s = m.size();
    printf("%s: \t[", name);
    for (int i=0; i < s.height; i++)
    {
        for (int j=0; j < s.width; j++)
            printf("%.2f, ", m.at<double>(i,j));
        // if(i+1 != s.height)
            // cout << endl << "\t ";
    }
    cout << "]\n\n";
}

void printPoints(const intrinsicCalibration inCal)
{
    for (auto v:inCal.objectPoints)
    {
        cout << "object " << v.size() << endl << "[";
        for (auto p:v)
             cout << " " << p << " ";
        cout << endl << endl;
    }
    for (auto v:inCal.imagePoints)
    {
        cout << "image " << v.size() << endl << "[";
        for (auto p:v)
             cout << " " << p << " ";
        cout << endl << endl;
    }
}

bool pathCheck(const string& path)
{
    DIR* dir = opendir(path.c_str());
    if (dir)              // If the path is an actual directory
    {
        closedir(dir);
        return true;
    }
    else                  // Directory does not exist
        return false;
}


//-------------------------Calibration functions------------------------------//
double computeReprojectionErrors(intrinsicCalibration &inCal)
{
    vector<Point2f> imagePoints2;
    int i, totalPoints = 0;
    double totalErr = 0, err;
    inCal.reprojErrs.resize(inCal.objectPoints.size());
    for( i = 0; i < (int)inCal.objectPoints.size(); i++ )
    {
        projectPoints(Mat(inCal.objectPoints[i]), inCal.rvecs[i], inCal.tvecs[i],
                      inCal.cameraMatrix, inCal.distCoeffs, imagePoints2);
        err = norm(Mat(inCal.imagePoints[i]), Mat(imagePoints2), CV_L2);
        int n = (int)inCal.objectPoints[i].size();
        inCal.reprojErrs[i] = (float)std::sqrt(err*err/n);
        totalErr += err*err;
        totalPoints += n;
    }
    return std::sqrt(totalErr/totalPoints);
}

void calcChessboardCorners(Settings s, vector<Point3f>& objectPointsBuf)
{
    for( int i = 0; i < s.boardSize.height; i++ )
        for( int j = 0; j < s.boardSize.width; j++ )
            objectPointsBuf.push_back(Point3f(float(j*s.squareSize),
                                      float(i*s.squareSize), 0));
}

//given the set of aruco markers detected, determine the corresponding 3d object points
void calcArucoCorners(vector<Point2f> &imagePointsBuf, vector<Point3f> &objectPointsBuf,
                            const vector<Marker> &markers_detected,
                            const MarkerMap &map)
{
    imagePointsBuf.clear();
    objectPointsBuf.clear();
    //for each detected marker
    for(size_t i=0;i<markers_detected.size();i++){
        int markerIndex=-1;
        //find the marker in the map
        for(size_t j=0;j<map.size() && markerIndex==-1;j++)
            if (map[j].id==markers_detected[i].id ) markerIndex=j;
        if (markerIndex!=-1){
            for(int j=0;j<4;j++){
                imagePointsBuf.push_back(markers_detected[i][j]);
                objectPointsBuf.push_back(map[markerIndex][j]);
            }
        }
    }
    //cout<<inCal.objectPoints.size()/4<<" markers detected"<<endl;
}

vector<Point3f> getIntPoints(Settings s, vector<Point3f> &points, int index){
    vector<Point3f> intPoints;
    string plane = s.planeList[index];

    switch (plane) {
        case "XY":
            for(auto p:points) intPoints.push_back(Point3f((p.x+1000)/125, (p.y+1000)/125, 0));
            break;
        case "YZ":
            for(auto p:points) intPoints.push_back(Point3f(0, (p.y+1000)/125, (-p.x+1000)/125));
            break;
        case "XZ":
            for(auto p:points) intPoints.push_back(Point3f((p.x+1000)/125, 0, (-p.y+1000)/125));
            break;
    }
    return intPoints;
}

void getSharedPoints(intrinsicCalibration &inCal, intrinsicCalibration &inCal2)
{
    // pointers to make code more legible
    vector<Point3f> *oPoints, *oPoints2;
    vector<Point2f> *iPoints, *iPoints2;
    int shared;     //index of a shared object point

    //for each objectPoints vector in overall objectPoints vector of vectors
    for (int i=0; i<(int)inCal.objectPoints.size(); i++)
    {
        vector<Point3f> sharedObjectPoints;
        vector<Point2f> sharedImagePoints, sharedImagePoints2;   //shared image points for each inCal

        oPoints = &inCal.objectPoints.at(i);
        oPoints2 = &inCal2.objectPoints.at(i);
        iPoints  = &inCal.imagePoints.at(i);
        iPoints2 = &inCal2.imagePoints.at(i);
        for (int j=0; j<(int)oPoints->size(); j++)
        {
            for (shared=0; shared<(int)oPoints2->size(); shared++)
                if (oPoints->at(j) == oPoints2->at(shared)) break;
            if (shared != (int)oPoints2->size())       //object point is shared
            {
                sharedObjectPoints.push_back(oPoints->at(j));
                sharedImagePoints.push_back(iPoints->at(j));
                sharedImagePoints2.push_back(iPoints2->at(shared));
            }
        }
        *oPoints = sharedObjectPoints;
        *oPoints2 = sharedObjectPoints;
        *iPoints = sharedImagePoints;
        *iPoints2 = sharedImagePoints2;
    }
}

void drawMarker(Settings s, Marker &marker, Mat &img, Scalar color, int lineWidth, cv::Point3f printPoint, int corner) {
    // Draw a rectangle around the marker
    // (marker)[x] is coordinate of corner on image
    cv::line(img, (marker)[0], (marker)[1], color, lineWidth, CV_AA);
    cv::line(img, (marker)[1], (marker)[2], color, lineWidth, CV_AA);
    cv::line(img, (marker)[2], (marker)[3], color, lineWidth, CV_AA);
    cv::line(img, (marker)[3], (marker)[0], color, lineWidth, CV_AA);

    auto p2=Point2f(lineWidth, lineWidth);
    cv::rectangle(img, (marker)[corner] - p2, (marker)[corner] + p2,  Scalar(255 - color[0], 255 - color[1], 255 - color[2], 255), lineWidth, CV_AA);

    // Determine the center point
    Point cent(0, 0);
    for (int i = 0; i < 4; i++) {
        cent.x += (marker)[i].x;
        cent.y += (marker)[i].y;
    }
    cent.x /= 4.;
    cent.y /= 4.;

    if (s.showArucoCoords) {        // draw the input printPoint, which is the marker coordinate
        cv::rectangle(img, (marker)[corner] - p2, (marker)[corner] + p2,  Scalar(255 - color[0], 255 - color[1], 255 - color[2], 255), lineWidth, CV_AA);

        char p[100];
        sprintf(p, "(%d,%d,%d)", (int)printPoint.x, (int)printPoint.y, (int)printPoint.z);
        putText(img, p, cent, FONT_HERSHEY_SIMPLEX, .5f, Scalar(255 - color[0], 255 - color[1], 255 - color[2], 255), 2);
    }
    else {                          // draw the ID number
        char cad[100];
        sprintf(cad, "id=%d", marker.id);
        putText(img, cad, cent, FONT_HERSHEY_SIMPLEX,  std::max(0.5f,float(lineWidth)*0.3f), Scalar(255 - color[0], 255 - color[1], 255 - color[2], 255), std::max(lineWidth,2));
    }
}

void drawArucoMarkers(Settings s, Mat &img, vector<Point3f> &objectPointsBuf,
                      vector<Marker> detectedMarkers,
                      vector<int> markersFromSet, int index)
{
    // corner is the index of the corner to be draw
    // each marker's points are stored in a list:
    //    [upper left, upper right, lower right, lower left]
    int corner;
    string plane = s.planeList(index);
    switch (plane) {
        case "XY":
            corner = 0;
            break;
        case "YZ":
            corner = 2;
            break;
        case "XZ":
            corner = 3;
            break;
        default:
            corner = 3;
    }
    // Color for marker to be drawn in draw function
    Scalar color = Scalar(0,0,0);
    color[index] = 255;

    // Draws each detected markers onto the image
    // Each marker has 4 detected object points, so loop through size of inCal.objectPoints/4
    int markerIndex;
    for (int k = 0; k < (int)objectPointsBuf.size()/4; k++) {
        markerIndex = markersFromSet[k];
        drawMarker(s, detectedMarkers[markerIndex], img, color, max(float(1.f),1.5f*float(img.cols)/1000.f),
                   objectPointsBuf[k*4+corner], corner);
    }
    img.copyTo(img);
}

void chessboardDetect(Settings s, Mat &img, intrinsicCalibration &inCal)
{
    //create grayscale copy for cornerSubPix function
    Mat imgGray;
    cvtColor(img, imgGray, COLOR_BGR2GRAY);

    //buffer to store points for each image
    vector<Point2f> imagePointsBuf;
    vector<Point3f> objectPointsBuf;
    bool found = findChessboardCorners( img, s.boardSize, imagePointsBuf,
        CV_CALIB_CB_ADAPTIVE_THRESH | CV_CALIB_CB_FILTER_QUADS | CV_CALIB_CB_FAST_CHECK |
        CV_CALIB_CB_NORMALIZE_IMAGE);
    if (found)
    {
        cornerSubPix(imgGray, imagePointsBuf, Size(11,11), Size(-1,-1),
                     TermCriteria( CV_TERMCRIT_EPS+CV_TERMCRIT_ITER, 30, 0.1 ));

        //add these image points to the overall calibration vector
        inCal.imagePoints.push_back(imagePointsBuf);

        //find the corresponding objectPoints
        calcChessboardCorners(s, objectPointsBuf);
        inCal.objectPoints.push_back(objectPointsBuf);
        drawChessboardCorners(img, s.boardSize, Mat(imagePointsBuf), found);
    }
}

void arucoDetect(Settings s, Mat &img, intrinsicCalibration &inCal, int vectorIndex)
{
    MarkerDetector TheMarkerDetector;
    //set specific parameters for this configuration
    MarkerDetector::Params params;
    params._borderDistThres=.01;//acept markers near the borders
    params._maxSize=0.9;
    params._thresParam1=5;
    params._thresParam1_range=10;//search in wide range of values for param1
    params._cornerMethod=MarkerDetector::SUBPIX;//use subpixel corner refinement
    params._subpix_wsize= (10./2000.)*float(img.cols) ;//search corner subpix in a window area
    //cout<<params._subpix_wsize<<" "<<float(img.cols)<<endl;
    TheMarkerDetector.setParams(params);//set the params above

    //pointers to the overall imagePoints and objectPoints vectors for the image
    //the points from all three config maps will be added to the image vectors

    vector<Point2f> *imgImagePoints;
    vector<Point3f> *imgObjectPoints;

    if (s.mode != Settings::PREVIEW) {
        imgImagePoints = &inCal.imagePoints.at(vectorIndex);
        imgObjectPoints = &inCal.objectPoints.at(vectorIndex);
    }

    //for each config file, detect its markers and draw them
    for(int j=0; j < s.nConfigs; j++) {
        MarkerMap MarkerMapConfig = s.configList[j];
        TheMarkerDetector.setDictionary(MarkerMapConfig.getDictionary());
        vector<Marker> detectedMarkers;
        vector<int> markersFromSet;

        //point buffers to store points for each config
        vector<Point2f> imagePointsBuf;
        vector<Point3f> objectPointsBuf;

        // detect the markers using MarkerDetector object
        detectedMarkers = TheMarkerDetector.detect(img);
        markersFromSet = MarkerMapConfig.getIndices(detectedMarkers);
        calcArucoCorners(imagePointsBuf,objectPointsBuf,detectedMarkers,MarkerMapConfig);

        // Convert the object points to int values. This also compensates for box geometry;
        // box plane is based on config's index value j (xy, yz, xz)
        // Because of this, our aruco box config list file must be in order 3, 2, 1
        objectPointsBuf = getIntPoints(s, objectPointsBuf, j);

        // add the point buffers to the overall calibration vectors
        if(objectPointsBuf.size()>0 && s.mode != Settings::PREVIEW){
            for (auto p:imagePointsBuf) imgImagePoints->push_back(p);
            for (auto p:objectPointsBuf) imgObjectPoints->push_back(p);
        }
        drawArucoMarkers(s, img, objectPointsBuf, detectedMarkers, markersFromSet, j);
    }
}


//--------------------Running and saving functions----------------------------//

static void undistortImages(Settings s, intrinsicCalibration &inCal)
{
    Mat img, Uimg;
    char imgSave[1000];

    bool save = false;
    if(s.undistortedPath != "0" && s.mode != Settings::PREVIEW)
    {
        if( pathCheck(s.undistortedPath) )
            save = true;
        else
            printf("\nUndistorted images could not be saved. Invalid path: %s\n", s.undistortedPath.c_str());
    }

    namedWindow("Undistorted", CV_WINDOW_AUTOSIZE);
    for( int i = 0; i < s.nImages; i++ )
    {
        img = s.imageSetup(i);
        undistort(img, Uimg, inCal.cameraMatrix, inCal.distCoeffs);

        if(save)
        {
            sprintf(imgSave, "%sundistorted_%d.jpg", s.undistortedPath.c_str(), i);
            imwrite(imgSave, Uimg);
        }

        if(s.showUndistorted)
        {
            imshow("Undistorted", Uimg);
            char c = (char)waitKey();
            if( (c & 255) == 27 || c == 'q' || c == 'Q' )   //escape key or 'q'
                break;
        }
    }
    destroyWindow("Undistorted");
}

void rectifyImages(Settings s, intrinsicCalibration &inCal,
                   intrinsicCalibration &inCal2, stereoCalibration &sterCal)
{
    Mat rmap[2][2];

    //Precompute maps for cv::remap()
    initUndistortRectifyMap(inCal.cameraMatrix, inCal.distCoeffs, sterCal.R1,
                        sterCal.P1, s.imageSize, CV_16SC2, rmap[0][0], rmap[0][1]);
    initUndistortRectifyMap(inCal2.cameraMatrix, inCal2.distCoeffs, sterCal.R2,
                        sterCal.P2, s.imageSize, CV_16SC2, rmap[1][0], rmap[1][1]);

    Mat canvas, rimg, cimg;
    double sf = 600. / MAX(s.imageSize.width, s.imageSize.height);
    int w = cvRound(s.imageSize.width * sf);
    int h = cvRound(s.imageSize.height * sf);
    canvas.create(h, w*2, CV_8UC3);

    // buffer for image filename
    char imgSave[1000];
    const char *view;

    bool save = false;
    if(s.rectifiedPath != "0")
    {
        if( pathCheck(s.undistortedPath) )
            save = true;
        else
            printf("\nRectified images could not be saved. Invalid path: %s\n", s.rectifiedPath.c_str());
    }

    namedWindow("Rectified", CV_WINDOW_AUTOSIZE);
    for( int i = 0; i < s.nImages/2; i++ )
    {
        for( int k = 0; k < 2; k++ )
        {
            Mat img = imread(s.imageList[i*2+k], 0), rimg, cimg;
            if (img.cols>1280) resize(img, img, Size(), 0.5, 0.5);
            remap(img, rimg, rmap[k][0], rmap[k][1], CV_INTER_LINEAR);

            // if a path for rectified images has been provided, save them to this path
            if (save)
            {
                view = "left";
                if (k == 1) view = "right";
                sprintf(imgSave, "%s%s_rectified_%d.jpg", s.rectifiedPath.c_str(), view, i);
                imwrite(imgSave, rimg);
            }

            cvtColor(rimg, cimg, COLOR_GRAY2BGR);
            Mat canvasPart = canvas(Rect(w*k, 0, w, h));
            resize(cimg, canvasPart, canvasPart.size(), 0, 0, CV_INTER_AREA);

            Rect vroi(cvRound(sterCal.validRoi[k].x*sf), cvRound(sterCal.validRoi[k].y*sf),
                      cvRound(sterCal.validRoi[k].width*sf), cvRound(sterCal.validRoi[k].height*sf));
            rectangle(canvasPart, vroi, Scalar(0,0,255), 3, 8);
        }
        for( int j = 0; j < canvas.rows; j += 16 )
            line(canvas, Point(0, j), Point(canvas.cols, j), Scalar(0, 255, 0), 1, 8);

        if (s.showRectified)
        {
            imshow("Rectified", canvas);
            char c = (char)waitKey();
            if( c == 27 || c == 'q' || c == 'Q' )
                break;
        }
    }
    destroyWindow("Rectified");
}

bool runIntrinsicCalibration(Settings s, intrinsicCalibration &inCal)
{
    if (s.useIntrinsicInput)     //precalculated intrinsic have been inputted. Use these
    {
        inCal.cameraMatrix = s.intrinsicInput.cameraMatrix;
        inCal.distCoeffs = s.intrinsicInput.distCoeffs;
        calibrateCamera(inCal.objectPoints, inCal.imagePoints, s.imageSize,
                        inCal.cameraMatrix, inCal.distCoeffs,
                        inCal.rvecs, inCal.tvecs, s.flag | CV_CALIB_USE_INTRINSIC_GUESS);

    } else {                //else, create empty matrices to be calculated
        inCal.cameraMatrix = Mat::eye(3, 3, CV_64F);
        inCal.distCoeffs = Mat::zeros(8, 1, CV_64F);

        calibrateCamera(inCal.objectPoints, inCal.imagePoints, s.imageSize,
                        inCal.cameraMatrix, inCal.distCoeffs,
                        inCal.rvecs, inCal.tvecs, s.flag);
    }

    // if( flag & CV_CALIB_FIX_ASPECT_RATIO )
    //     inCal.cameraMatrix.at<double>(0,0) = aspectRatio;

    bool ok = checkRange(inCal.cameraMatrix) && checkRange(inCal.distCoeffs);
    inCal.totalAvgErr = computeReprojectionErrors(inCal);
    return ok;
}


stereoCalibration runStereoCalibration(Settings s, intrinsicCalibration &inCal, intrinsicCalibration &inCal2)
{
    stereoCalibration sterCal;
    if (s.useIntrinsicInput)     //precalculated intrinsic have been inputted. Use these
    {
        inCal.cameraMatrix = s.intrinsicInput.cameraMatrix;
        inCal2.cameraMatrix = s.intrinsicInput.cameraMatrix;
        inCal.distCoeffs = s.intrinsicInput.distCoeffs;
        inCal2.distCoeffs = s.intrinsicInput.distCoeffs;
    }

    if (s.calibrationPattern != Settings::CHESSBOARD)       //aruco pattern
        getSharedPoints(inCal, inCal2);

    double err = stereoCalibrate(
               inCal.objectPoints, inCal.imagePoints, inCal2.imagePoints,
               inCal.cameraMatrix, inCal.distCoeffs,
               inCal2.cameraMatrix, inCal2.distCoeffs,
               s.imageSize, sterCal.R, sterCal.T, sterCal.E, sterCal.F,
               TermCriteria(CV_TERMCRIT_ITER+CV_TERMCRIT_EPS, 1000, 1e-10),
               CV_CALIB_FIX_INTRINSIC);

    printf("\nStereo reprojection error = %.4f\n", err);

    stereoRectify(inCal.cameraMatrix, inCal.distCoeffs,
                 inCal2.cameraMatrix, inCal2.distCoeffs,
                 s.imageSize, sterCal.R, sterCal.T, sterCal.R1, sterCal.R2,
                 sterCal.P1, sterCal.P2, sterCal.Q,
                 CALIB_ZERO_DISPARITY, 1, s.imageSize,
                 &sterCal.validRoi[0], &sterCal.validRoi[1]);

    rectifyImages(s, inCal, inCal2, sterCal);
    return sterCal;
}

static void undistortCheck(Settings s, Mat &img, bool &undistortPreview)
{
    if (undistortPreview)
    {
        if (s.useIntrinsicInput)
        {
            Mat temp = img.clone();
            undistort(temp, img, s.intrinsicInput.cameraMatrix,
                      s.intrinsicInput.distCoeffs);
        } else {
            cerr << "\nUndistorted preview requires intrinsic input.\n";
            undistortPreview = !undistortPreview;
        }
    }
}

void runCalibrationAndSave(Settings s, intrinsicCalibration &inCal, intrinsicCalibration &inCal2)
{
    bool ok;
    if (s.mode == Settings::STEREO) {         // stereo calibration
        if (!s.useIntrinsicInput)
        {
        ok = runIntrinsicCalibration(s, inCal);
        printf("%s for left. Avg reprojection error = %.4f\n",
                ok ? "\nIntrinsic calibration succeeded" : "\nIntrinsic calibration failed",
                inCal.totalAvgErr);
        ok = runIntrinsicCalibration(s, inCal2);
        printf("%s for right. Avg reprojection error = %.4f\n",
                ok ? "\nIntrinsic calibration succeeded" : "\nIntrinsic calibration failed",
                inCal2.totalAvgErr);
        } else
            ok = true;

        stereoCalibration sterCal = runStereoCalibration(s, inCal, inCal2);

        s.saveExtrinsics(sterCal);

    } else {                        // intrinsic calibration
        ok = runIntrinsicCalibration(s, inCal);
        printf("%s. Avg reprojection error = %.4f\n",
                ok ? "\nIntrinsic calibration succeeded" : "\nIntrinsic calibration failed",
                inCal.totalAvgErr);

        if( ok ) {
            undistortImages(s, inCal);
            s.saveIntrinsics(inCal);
        }
    }
}


int calibrateWithSettings( const string inputSettingsFile )
{
    Settings s;
    FileStorage fs(inputSettingsFile, FileStorage::READ); // Read the settings
    if (!fs.isOpened())
    {
        cerr << "Could not open the configuration file: \"" << inputSettingsFile << "\"" << endl;
        return -1;
    }
    fs["Settings"] >> s;
    fs.release();                                         // close Settings file

    if (!s.goodInput)
    {
        cerr << "Invalid input detected. Application stopping. " << endl;
        return -1;
    }

    //struct to store calibration parameters
    intrinsicCalibration inCal, inCal2;
    intrinsicCalibration *currentInCal = &inCal;

    int size = (s.mode == Settings::STEREO) ? s.nImages/2 : s.nImages;
    if (s.calibrationPattern != Settings::CHESSBOARD) {
        inCal.imagePoints.resize(size);
        inCal.objectPoints.resize(size);
        inCal2.imagePoints.resize(size);
        inCal2.objectPoints.resize(size);
    }

    int vectorIndex = -1;
    bool undistortPreview = false;

    char imgSave[1000];
    bool save = false;
    if(s.detectedPath != "0" && s.mode != Settings::PREVIEW)
    {
        if( pathCheck(s.detectedPath) )
            save = true;
        else
            printf("\nDetected images could not be saved. Invalid path: %s\n", s.detectedPath.c_str());
    }

    namedWindow("Detected", CV_WINDOW_AUTOSIZE);
    // For each image in the image list
    for(int i = 0;;i++)
    {
        //this is unecessarily complicated, but it works
        if (i%2 == 0) {
            currentInCal = &inCal;
            vectorIndex++;
        } else if (s.mode == Settings::STEREO)
            currentInCal = &inCal2;
        else
            vectorIndex++;

        //set up the view
        Mat img = s.imageSetup(i);

        // if there is no data, the capture has closed or the photos have run out
        if(!img.data)
        {
            if((int)inCal.imagePoints.size() > 0) {
                destroyWindow("Detected");
                runCalibrationAndSave(s, inCal, inCal2);
            }
            break;
        }
        s.imageSize = img.size();

        //Detect the pattern in the image, adding data to the imagePoints
        //and objectPoints calibration parameters
        if(s.calibrationPattern == Settings::CHESSBOARD)
            chessboardDetect(s, img, *currentInCal);
        else
            arucoDetect(s, img, *currentInCal, vectorIndex);

        if (s.mode == Settings::PREVIEW)
            undistortCheck(s, img, undistortPreview);

        if(save)
        {
            sprintf(imgSave, "%sdetected_%d.jpg", s.detectedPath.c_str(), i);
            imwrite(imgSave, img);
        }

        imshow("Detected", img);

        // If wait setting is true, wait till next key press (waitkey(0)). Otherwise, wait 50 ms
        char c = (char)waitKey(s.wait ? 0: 50);

        if (c == 'u')
            undistortPreview = !undistortPreview;
        if (c == 'c' && s.mode == Settings::PREVIEW)
            s.showArucoCoords = !s.showArucoCoords;
        else if( (c & 255) == 27 || c == 'q' || c == 'Q' )
            break;
    }
    destroyWindow("Detected");
    return 0;
}
