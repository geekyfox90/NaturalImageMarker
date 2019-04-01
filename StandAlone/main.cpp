/**
 * @copyright Copyright (c) 2017 B-com http://www.b-com.com/
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <iostream>
#include <vector>
#include <numeric>
#include <string>
#include <functional>

#include <boost/log/core.hpp>

using namespace std;
#include "SolARModuleOpencv_traits.h"
#include "SolARModuleTools_traits.h"

#include "xpcf/xpcf.h"

#include "api/input/devices/ICamera.h"
#include "api/input/files/IMarker2DNaturalImage.h"
#include "api/features/IKeypointDetector.h"
#include "api/features/IKeypointDetectorRegion.h"
#include "api/features/IDescriptorMatcher.h"
#include "api/features/IDescriptorsExtractor.h"
#include "api/features/IMatchesFilter.h"
#include "api/features/IKeypointsReIndexer.h"
#include "api/geom/IImage2WorldMapper.h"
#include "api/geom/IProject.h"
#include "api/geom/IUnproject.h"
#include "api/solver/pose/I3DTransformSACFinderFrom2D3D.h"
#include "api/tracking/IOpticalFlowEstimator.h"
#include "api/display/I2DOverlay.h"
#include "api/display/I3DOverlay.h"
#include "api/display/IImageViewer.h"


#include "core/Log.h"

#include <boost/timer/timer.hpp>
#include <boost/chrono.hpp>

using namespace SolAR;
using namespace SolAR::MODULES::OPENCV;
using namespace SolAR::MODULES::TOOLS;
using namespace SolAR::datastructure;
using namespace SolAR::api;
namespace xpcf = org::bcom::xpcf;

#include <string>

#include <thread> // std::this_thread::sleep_for
#include <chrono> // std::chrono::seconds

#define TRACKING

int updateTrackedPointThreshold = 500;

int main(int argc, char *argv[])
{

#ifdef NDEBUG
	boost::log::core::get()->set_logging_enabled(false);
#endif

	//    SolARLog::init();
	LOG_ADD_LOG_TO_CONSOLE();
	LOG_INFO("program is running");

    try {
        /* instantiate component manager*/
        /* this is needed in dynamic mode */
        SRef<xpcf::IComponentManager> xpcfComponentManager = xpcf::getComponentManagerInstance();

        if (xpcfComponentManager->load("conf_NaturalImageMarker.xml") != org::bcom::xpcf::_SUCCESS)
        {
            LOG_ERROR("Failed to load the configuration file conf_NaturalImageMarker.xml", argv[1])
            return -1;
        }
        // declare and create components
        LOG_INFO("Start creating components");

        auto camera = xpcfComponentManager->create<SolARCameraOpencv>()->bindTo<input::devices::ICamera>();
        auto marker = xpcfComponentManager->create<SolARMarker2DNaturalImageOpencv>()->bindTo<input::files::IMarker2DNaturalImage>();
        auto kpDetector = xpcfComponentManager->create<SolARKeypointDetectorOpencv>()->bindTo<features::IKeypointDetector>();
        auto kpDetectorRegion = xpcfComponentManager->create<SolARKeypointDetectorRegionOpencv>()->bindTo<features::IKeypointDetectorRegion>();
        auto descriptorExtractor = xpcfComponentManager->create<SolARDescriptorsExtractorAKAZE2Opencv>()->bindTo<features::IDescriptorsExtractor>();
        auto matcher = xpcfComponentManager->create<SolARDescriptorMatcherKNNOpencv>()->bindTo<features::IDescriptorMatcher>();
        auto basicMatchesFilter = xpcfComponentManager->create<SolARBasicMatchesFilter>()->bindTo<features::IMatchesFilter>();
        auto geomMatchesFilter = xpcfComponentManager->create<SolARGeometricMatchesFilterOpencv>()->bindTo<features::IMatchesFilter>();
        auto keypointsReindexer = xpcfComponentManager->create<SolARKeypointsReIndexer>()->bindTo<features::IKeypointsReIndexer>();
        auto poseEstimationPlanar = xpcfComponentManager->create<SolARPoseEstimationPlanarPointsOpencv>()->bindTo<solver::pose::I3DTransformSACFinderFrom2D3D>();
        auto img_mapper = xpcfComponentManager->create<SolARImage2WorldMapper4Marker2D>()->bindTo<geom::IImage2WorldMapper>();
        auto opticalFlowEstimator = xpcfComponentManager->create<SolAROpticalFlowPyrLKOpencv>()->bindTo<tracking::IOpticalFlowEstimator>();
        auto projection = xpcfComponentManager->create<SolARProjectOpencv>()->bindTo<geom::IProject>();
        auto unprojection = xpcfComponentManager->create<SolARUnprojectPlanarPointsOpencv>()->bindTo<geom::IUnproject>();
        auto overlay2DComponent = xpcfComponentManager->create<SolAR2DOverlayOpencv>()->bindTo<display::I2DOverlay>();
        auto overlay3DComponent = xpcfComponentManager->create<SolAR3DOverlayBoxOpencv>()->bindTo<display::I3DOverlay>();
        auto imageViewerKeypoints = xpcfComponentManager->create<SolARImageViewerOpencv>("keypoints")->bindTo<display::IImageViewer>();
        auto imageViewerResult = xpcfComponentManager->create<SolARImageViewerOpencv>()->bindTo<display::IImageViewer>();

        // Declare data structures used to exchange information between components
        SRef<Image> refImage, camImage, previousCamImage, kpImageCam;
        std::vector<SRef<Keypoint>> refKeypoints;
        SRef<DescriptorBuffer> refDescriptors;
        std::vector<SRef<Point3Df>> markerWorldCorners;
        std::vector<SRef<Point2Df>> projectedMarkerCorners;
        std::vector<SRef<Point2Df>> imagePoints_inliers;
        std::vector<SRef<Point3Df>> worldPoints_inliers;
        Transform3Df pose;
        bool valid_pose = false;
        bool isTrack = false;
        bool needNewTrackedPoints = false;

        // load marker
        LOG_INFO("LOAD MARKER IMAGE ");
        marker->loadMarker();
        marker->getWorldCorners(markerWorldCorners);

        marker->getImage(refImage);

        // detect keypoints in reference image
        kpDetector->detect(refImage, refKeypoints);

        // extract descriptors in reference image
        descriptorExtractor->extract(refImage, refKeypoints, refDescriptors);

#ifndef NDEBUG
        // display keypoints in reference image
        // copy reference image
        SRef<Image> kpImage = refImage->copy();
        // draw circles on keypoints

        overlay2DComponent->drawCircles(refKeypoints, kpImage);
        // displays the image with circles in an imageviewer
        imageViewerKeypoints->display(kpImage);
#endif

        if (camera->start() != FrameworkReturnCode::_SUCCESS) // videoFile
        {
            LOG_ERROR("Camera cannot start");
            return -1;
        }

        // initialize overlay 3D component with the camera intrinsec parameters (please refeer to the use of intrinsec parameters file)
        overlay3DComponent->setCameraParameters(camera->getIntrinsicsParameters(), camera->getDistorsionParameters());

        // initialize pose estimation based on planar points with the camera intrinsec parameters (please refeer to the use of intrinsec parameters file)
        poseEstimationPlanar->setCameraParameters(camera->getIntrinsicsParameters(), camera->getDistorsionParameters());

        // initialize projection component with the camera intrinsec parameters (please refeer to the use of intrinsec parameters file)
        projection->setCameraParameters(camera->getIntrinsicsParameters(), camera->getDistorsionParameters());

        // initialize unprojection component with the camera intrinsec parameters (please refeer to the use of intrinsec parameters file)
        unprojection->setCameraParameters(camera->getIntrinsicsParameters(), camera->getDistorsionParameters());

        // initialize image mapper with the reference image size and marker size
        img_mapper->bindTo<xpcf::IConfigurable>()->getProperty("digitalWidth")->setIntegerValue(refImage->getSize().width);
        img_mapper->bindTo<xpcf::IConfigurable>()->getProperty("digitalHeight")->setIntegerValue(refImage->getSize().height);
        img_mapper->bindTo<xpcf::IConfigurable>()->getProperty("worldWidth")->setFloatingValue(marker->getSize().width);
        img_mapper->bindTo<xpcf::IConfigurable>()->getProperty("worldHeight")->setFloatingValue(marker->getSize().height);

        // to count the average number of processed frames per seconds
        clock_t start, end;
        int count = 0;
        start = clock();
        isTrack = false;

        // get images from camera in loop, and display them
        while (true)
        {
            valid_pose = false;
            count++;

            if (camera->getNextImage(camImage) == SolAR::FrameworkReturnCode::_ERROR_)
                break;

            /* we declare here the Solar datastucture we will need for homography*/
            std::vector<SRef<Keypoint>> camKeypoints; // where to store detected keypoints in ref image and camera image
            SRef<DescriptorBuffer> camDescriptors;
            std::vector<DescriptorMatch> matches;
            std::vector<SRef<Point2Df>> refMatched2Dpoints, camMatched2Dpoints;
            std::vector<SRef<Point3Df>> ref3Dpoints;

            if (!isTrack) // We estimate the pose by matching marker planar keypoints and current image keypoints and by estimating the pose based on planar points
            {
                //detect natural marker from features points
                // detect keypoints in camera image
                kpDetector->detect(camImage, camKeypoints);

#ifndef NDEBUG
                kpImageCam = camImage->copy();
                //overlay2DComponent->drawCircles(camKeypoints, kpImageCam);
#endif

                /* extract descriptors in camera image*/
                descriptorExtractor->extract(camImage, camKeypoints, camDescriptors);

                /*compute matches between reference image and camera image*/
                matcher->match(refDescriptors, camDescriptors, matches);

                /* filter matches to remove redundancy and check geometric validity */
                basicMatchesFilter->filter(matches, matches, refKeypoints, camKeypoints);
                geomMatchesFilter->filter(matches, matches, refKeypoints, camKeypoints);


                /*we consider that, if we have less than 10 matches (arbitrarily), we can't compute homography for the current frame */
                if (matches.size() > 10)
                {
                    // reindex the keypoints with established correspondence after the matching
                    keypointsReindexer->reindex(refKeypoints, camKeypoints, matches, refMatched2Dpoints, camMatched2Dpoints);

                    // mapping to 3D points
                    img_mapper->map(refMatched2Dpoints, ref3Dpoints);

                    // Estimate the pose from the 2D-3D planar correspondence
                    if (poseEstimationPlanar->estimate(camMatched2Dpoints, ref3Dpoints, imagePoints_inliers, worldPoints_inliers, pose) != FrameworkReturnCode::_SUCCESS)
                    {
                        valid_pose = false;
                        LOG_DEBUG("Wrong homography for this frame");
                    }
                    else
                    {
#ifdef TRACKING
                        isTrack = true;
						needNewTrackedPoints = true;
#endif
                        valid_pose = true;
                        previousCamImage= camImage->copy();
                        LOG_INFO("Start tracking", pose.matrix());
                    }
                }
            }
            else // We track planar keypoints and we estimate the pose based on a homography
            {
                std::vector<SRef<Point2Df>> trackedPoints, pts2D;
                std::vector<SRef<Point3Df>> pts3D;
                std::vector<unsigned char> status;
                std::vector<float> err;

                // tracking 2D-2D
                opticalFlowEstimator->estimate(previousCamImage, camImage, imagePoints_inliers, trackedPoints, status, err);

                for (int i = 0; i < status.size(); i++)
                {
                    if (status[i])
                    {
						pts2D.push_back(trackedPoints[i]);
						pts3D.push_back(worldPoints_inliers[i]);
                    }                    
                }

#ifndef NDEBUG
                kpImageCam = camImage->copy();
                overlay2DComponent->drawCircles(pts2D, kpImageCam);
#endif

                // calculate camera pose
                // Estimate the pose from the 2D-3D planar correspondence
                if (poseEstimationPlanar->estimate(pts2D, pts3D, imagePoints_inliers, worldPoints_inliers, pose) != FrameworkReturnCode::_SUCCESS)
                {
                    isTrack = false;
                    valid_pose = false;
                    needNewTrackedPoints = false;
                    LOG_INFO("Tracking lost");
                }
                else
                {
                    valid_pose = true;
                    previousCamImage = camImage->copy();
                    if (worldPoints_inliers.size() < updateTrackedPointThreshold)
                        needNewTrackedPoints = true;
                }
            }

#ifdef TRACKING
            if (needNewTrackedPoints)
            {
				imagePoints_inliers.clear();
				worldPoints_inliers.clear();
                std::vector<SRef<Keypoint>> newKeypoints;
                // Get the projection of the corner of the marker in the current image
                projection->project(markerWorldCorners, projectedMarkerCorners, pose);
#ifndef NDEBUG
                overlay2DComponent->drawContour(projectedMarkerCorners, kpImageCam);
#endif				
                // Detect the keypoints within the contours of the marker defined by the projected corners
                kpDetectorRegion->detect(camImage, projectedMarkerCorners, newKeypoints);
                
                for (auto keypoint : newKeypoints)
                    imagePoints_inliers.push_back(xpcf::utils::make_shared<Point2Df>(keypoint->getX(), keypoint->getY()));

                // get back the 3D positions of the detected keypoints in world space
                unprojection->unproject(imagePoints_inliers, worldPoints_inliers, pose);
				needNewTrackedPoints = false;
                LOG_DEBUG("Reinitialize points to track");
            }
#endif

            //draw a cube if the pose if valid
            if (valid_pose)
            {
                // We draw a box on the place of the recognized natural marker
#ifndef NDEBUG
                overlay3DComponent->draw(pose, kpImageCam);
#else
                overlay3DComponent->draw(pose, camImage);
#endif
            }

#ifndef NDEBUG
            if (imageViewerResult->display(kpImageCam) == SolAR::FrameworkReturnCode::_STOP)
#else
            if (imageViewerResult->display(camImage) == SolAR::FrameworkReturnCode::_STOP)
#endif
            break;
        }
        end = clock();
        double duration = double(end - start) / CLOCKS_PER_SEC;
        printf("\n\nElasped time is %.2lf seconds.\n", duration);
        printf("Number of processed frame per second : %8.2f\n", count / duration);
        std::cout << "this is the end..." << '\n';
    }
    catch (xpcf::Exception e)
    {
        LOG_ERROR("The following exception has been catch {}", e.what());
    }

	return 0;
}
