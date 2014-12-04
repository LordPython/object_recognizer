#include <ros/ros.h>
#include <geometry_msgs/Point32.h>
#include <image_transport/image_transport.h>
#include <image_transport/transport_hints.h>
#include <cv_bridge/cv_bridge.h>
#include <sensor_msgs/image_encodings.h>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/features2d/features2d.hpp>
#include <opencv2/calib3d/calib3d.hpp>

static const std::string camera_topic = "/softkinetic_camera/color";
static const std::string map_topic = "/softkinetic_camera/inverse_uv_map";

class ObjDetector {
private:
    // Feature detector
    cv::Ptr<cv::FeatureDetector> detector;
    // Feature descriptor extractor
    cv::Ptr<cv::DescriptorExtractor> extractor;
    // Feature matcher
    cv::Ptr<cv::DescriptorMatcher> matcher;

    // Object calibration image
    cv::Mat obj_img;
    // Object feature keypoints
    std::vector<cv::KeyPoint> obj_keypoints;
    // Object feature descriptors
    cv::Mat obj_descriptors;

    image_transport::ImageTransport it;
    image_transport::Subscriber image_sub;
    image_transport::Subscriber map_sub;

    bool haveImageFrame, haveMapFrame;
    sensor_msgs::ImageConstPtr lastImageFrame, lastMapFrame;
public:
    ObjDetector(ros::NodeHandle nh, std::string calib_image_name) : 
        it(nh), lastImageFrame(nullptr), lastMapFrame(nullptr),
        haveImageFrame(false), haveMapFrame(false)
    {
        // Setup feature detection/extraction/matching objects
        detector = cv::FeatureDetector::create("ORB");
        extractor = cv::DescriptorExtractor::create("FREAK");
        matcher = cv::DescriptorMatcher::create("BruteForce-Hamming");

        // Read calibration image
        obj_img = cv::imread(calib_image_name, CV_LOAD_IMAGE_GRAYSCALE);
        detector->detect(obj_img, obj_keypoints);
        extractor->compute(obj_img, obj_keypoints, obj_descriptors);

        // Subscribe to image/map messages from the camera
        image_sub = it.subscribe(camera_topic, 1, &ObjDetector::process_image_frame, this);
        map_sub   = it.subscribe(map_topic, 1, &ObjDetector::process_map_frame, this);
    }

    /**
     * find_object
     */
    geometry_msgs::Point32 find_object(const sensor_msgs::ImageConstPtr msg, const sensor_msgs::ImageConstPtr mapMsg) {
        cv_bridge::CvImagePtr img_ptr;

        try {
            img_ptr = cv_bridge::toCvCopy(msg, sensor_msgs::image_encodings::BGR8);
        } catch(cv_bridge::Exception& e) {
            ROS_ERROR("Failed to extract opencv image; cv_bridge exception: %s", e.what());
            exit(-1);
        }

        // Camera image converted to grayscale
        cv::Mat img;
        // Color camera image
        cv::Mat color;

        // Perform the Color->Gray conversion
        cv::cvtColor(img_ptr->image, img, CV_BGR2GRAY);
        // Copy the image
        color = img_ptr->image.clone();

        // Compute keypoints and descriptors
        std::vector<cv::KeyPoint> img_keypoints;
        cv::Mat img_descriptors;

        detector->detect(img, img_keypoints);
        extractor->compute(img, img_keypoints, img_descriptors);

        // Match keypoints from current frame to calibration image
        std::vector<cv::DMatch> matches;
        matcher->match(obj_descriptors, img_descriptors, matches);

        double max_dist = 0; double min_dist = 100;

        // Calculate min and max distances between keypoints
        //for( int i = 0; i < obj_descriptors.rows; i++ )
        for( int i = 0; i < matches.size(); i++ )
        { 
            double dist = matches[i].distance;
            if( dist < min_dist ) min_dist = dist;
            if( dist > max_dist ) max_dist = dist;
        }

        // The determination of what a "good" match is should probably
        // be revised.
        std::vector<cv::DMatch> good_matches;

        for( int i = 0; i < matches.size(); i++ )
        { 
            if( matches[i].distance < 3*min_dist )
                good_matches.push_back( matches[i]);
        }

        std::vector<cv::Point2f> obj;
        std::vector<cv::Point2f> scene;

        for( int i = 0; i < good_matches.size(); i++ )
        {
            //-- Get the keypoints from the good matches
            obj.push_back( obj_keypoints[ good_matches[i].queryIdx ].pt );
            scene.push_back( img_keypoints[ good_matches[i].trainIdx ].pt );
        }

        try {
            // Find homography from the calibration image to the current frame.
            cv::Mat H = cv::findHomography( obj, scene, CV_RANSAC );
            // Get the corners from the image_1 ( the object to be "detected" )
            std::vector<cv::Point2f> obj_corners(4);
            obj_corners[0] = cvPoint(0,0); 
            obj_corners[1] = cvPoint( obj_img.cols, 0 );
            obj_corners[2] = cvPoint( obj_img.cols, obj_img.rows ); 
            obj_corners[3] = cvPoint( 0, obj_img.rows );

            std::vector<cv::Point2f> scene_corners(4);

            // Transform object corners from calibration image to current frame
            perspectiveTransform( obj_corners, scene_corners, H);

            // Draw lines around object
            cv::line( color, scene_corners[0], scene_corners[1], cv::Scalar(0, 255, 0), 4 );
            cv::line( color, scene_corners[1], scene_corners[2], cv::Scalar( 0, 255, 0), 4 );
            cv::line( color, scene_corners[2], scene_corners[3], cv::Scalar( 0, 255, 0), 4 );
            cv::line( color, scene_corners[3], scene_corners[0], cv::Scalar( 0, 255, 0), 4 );
        } catch(cv::Exception e) {
            // Do nothing for now if we can't find a homography
        }

        
        // Show image (TODO: perhaps republish it instead?)
        cv::imshow("OUT", color);
        cv::waitKey(1);

        // Need to compute point here
        geometry_msgs::Point32 ret;
        ret.x = 0.0;
        ret.y = 0.0;
        ret.z = 0.0;
        return ret;
    }

    void detect() {
        if(lastImageFrame/* && lastMapFrame*/) {
            geometry_msgs::Point32 object_loc = find_object(lastImageFrame, lastMapFrame);
            // Need to publish/send out object location here.
            lastImageFrame = nullptr;
            lastMapFrame = nullptr;
        }
    }

    void process_image_frame(const sensor_msgs::ImageConstPtr& msg) {
        lastImageFrame = msg;
    }

    void process_map_frame(const sensor_msgs::ImageConstPtr& msg) {
        lastMapFrame = msg;
    }
};

int main(int argc, char** argv) {
    ros::init(argc, argv, "object_recognizer");
    ros::NodeHandle nh;

    if(argc < 2) {
        std::cout << "Missing "<< (2-argc) << " arguments!" << std::endl;
        return -1;
    }

    ObjDetector detector(nh, argv[1]);

    // Rate to check for new messages from the camera
    ros::Rate sampleRate(30); // 30HZ

    while(ros::ok()) {
        ros::spinOnce();
        detector.detect();
        sampleRate.sleep();
    }
}
