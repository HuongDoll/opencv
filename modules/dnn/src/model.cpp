// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.

#include "precomp.hpp"
#include <algorithm>
#include <iostream>
#include <utility>
#include <iterator>

#include <opencv2/imgproc.hpp>

namespace cv {
namespace dnn {

struct Model::Impl
{
//protected:
    Net    net;

    Size   size;
    Scalar mean;
    double  scale = 1.0;
    bool   swapRB = false;
    bool   crop = false;
    Mat    blob;
    std::vector<String> outNames;

public:
    virtual ~Impl() {}
    Impl() {}
    Impl(const Impl&) = delete;
    Impl(Impl&&) = delete;

    virtual Net& getNetwork() const { return const_cast<Net&>(net); }

    virtual void setPreferableBackend(Backend backendId) { net.setPreferableBackend(backendId); }
    virtual void setPreferableTarget(Target targetId) { net.setPreferableTarget(targetId); }

    /*virtual*/
    void initNet(const Net& network)
    {
        net = network;

        outNames = net.getUnconnectedOutLayersNames();
        std::vector<MatShape> inLayerShapes;
        std::vector<MatShape> outLayerShapes;
        net.getLayerShapes(MatShape(), 0, inLayerShapes, outLayerShapes);
        if (!inLayerShapes.empty() && inLayerShapes[0].size() == 4)
            size = Size(inLayerShapes[0][3], inLayerShapes[0][2]);
        else
            size = Size();
    }

    /*virtual*/
    void setInputParams(double scale_, const Size& size_, const Scalar& mean_,
                        bool swapRB_, bool crop_)
    {
        size = size_;
        mean = mean_;
        scale = scale_;
        crop = crop_;
        swapRB = swapRB_;
    }
    /*virtual*/
    void setInputSize(const Size& size_)
    {
        size = size_;
    }
    /*virtual*/
    void setInputMean(const Scalar& mean_)
    {
        mean = mean_;
    }
    /*virtual*/
    void setInputScale(double scale_)
    {
        scale = scale_;
    }
    /*virtual*/
    void setInputCrop(bool crop_)
    {
        crop = crop_;
    }
    /*virtual*/
    void setInputSwapRB(bool swapRB_)
    {
        swapRB = swapRB_;
    }

    /*virtual*/
    void processFrame(InputArray frame, OutputArrayOfArrays outs)
    {
        if (size.empty())
            CV_Error(Error::StsBadSize, "Input size not specified");

        blob = blobFromImage(frame, scale, size, mean, swapRB, crop);
        net.setInput(blob);

        // Faster-RCNN or R-FCN
        if (net.getLayer(0)->outputNameToIndex("im_info") != -1)
        {
            Mat imInfo(Matx13f(size.height, size.width, 1.6f));
            net.setInput(imInfo, "im_info");
        }
        net.forward(outs, outNames);
    }
};

Model::Model()
    : impl(makePtr<Impl>())
{
    // nothing
}

Model::Model(const String& model, const String& config)
    : Model()
{
    impl->initNet(readNet(model, config));
}

Model::Model(const Net& network)
    : Model()
{
    impl->initNet(network);
}

Net& Model::getNetwork_() const
{
    CV_DbgAssert(impl);
    return impl->getNetwork();
}

Model& Model::setPreferableBackend(Backend backendId)
{
    CV_DbgAssert(impl);
    impl->setPreferableBackend(backendId);
    return *this;
}
Model& Model::setPreferableTarget(Target targetId)
{
    CV_DbgAssert(impl);
    impl->setPreferableTarget(targetId);
    return *this;
}

Model& Model::setInputSize(const Size& size)
{
    CV_DbgAssert(impl);
    impl->setInputSize(size);
    return *this;
}

Model& Model::setInputMean(const Scalar& mean)
{
    CV_DbgAssert(impl);
    impl->setInputMean(mean);
    return *this;
}

Model& Model::setInputScale(double scale)
{
    CV_DbgAssert(impl);
    impl->setInputScale(scale);
    return *this;
}

Model& Model::setInputCrop(bool crop)
{
    CV_DbgAssert(impl);
    impl->setInputCrop(crop);
    return *this;
}

Model& Model::setInputSwapRB(bool swapRB)
{
    CV_DbgAssert(impl);
    impl->setInputSwapRB(swapRB);
    return *this;
}

void Model::setInputParams(double scale, const Size& size, const Scalar& mean,
                           bool swapRB, bool crop)
{
    CV_DbgAssert(impl);
    impl->setInputParams(scale, size, mean, swapRB, crop);
}

void Model::predict(InputArray frame, OutputArrayOfArrays outs) const
{
    CV_DbgAssert(impl);
    impl->processFrame(frame, outs);
}


ClassificationModel::ClassificationModel(const String& model, const String& config)
    : Model(model, config)
{
    // nothing
}

ClassificationModel::ClassificationModel(const Net& network)
    : Model(network)
{
    // nothing
}

std::pair<int, float> ClassificationModel::classify(InputArray frame)
{
    std::vector<Mat> outs;
    impl->processFrame(frame, outs);
    CV_Assert(outs.size() == 1);

    double conf;
    cv::Point maxLoc;
    minMaxLoc(outs[0].reshape(1, 1), nullptr, &conf, nullptr, &maxLoc);
    return {maxLoc.x, static_cast<float>(conf)};
}

void ClassificationModel::classify(InputArray frame, int& classId, float& conf)
{
    std::tie(classId, conf) = classify(frame);
}

KeypointsModel::KeypointsModel(const String& model, const String& config)
    : Model(model, config) {};

KeypointsModel::KeypointsModel(const Net& network) : Model(network) {};

std::vector<Point2f> KeypointsModel::estimate(InputArray frame, float thresh)
{

    int frameHeight = frame.rows();
    int frameWidth = frame.cols();
    std::vector<Mat> outs;

    impl->processFrame(frame, outs);
    CV_Assert(outs.size() == 1);
    Mat output = outs[0];

    const int nPoints = output.size[1];
    std::vector<Point2f> points;

    // If output is a map, extract the keypoints
    if (output.dims == 4)
    {
        int height = output.size[2];
        int width = output.size[3];

        // find the position of the keypoints (ignore the background)
        for (int n=0; n < nPoints - 1; n++)
        {
            // Probability map of corresponding keypoint
            Mat probMap(height, width, CV_32F, output.ptr(0, n));

            Point2f p(-1, -1);
            Point maxLoc;
            double prob;
            minMaxLoc(probMap, NULL, &prob, NULL, &maxLoc);
            if (prob > thresh)
            {
                p = maxLoc;
                p.x *= (float)frameWidth / width;
                p.y *= (float)frameHeight / height;
            }
            points.push_back(p);
        }
    }
    // Otherwise the output is a vector of keypoints and we can just return it
    else
    {
        for (int n=0; n < nPoints; n++)
        {
            Point2f p;
            p.x = *output.ptr<float>(0, n, 0);
            p.y = *output.ptr<float>(0, n, 1);
            points.push_back(p);
        }
    }
    return points;
}

SegmentationModel::SegmentationModel(const String& model, const String& config)
    : Model(model, config) {};

SegmentationModel::SegmentationModel(const Net& network) : Model(network) {};

void SegmentationModel::segment(InputArray frame, OutputArray mask)
{
    std::vector<Mat> outs;
    impl->processFrame(frame, outs);
    CV_Assert(outs.size() == 1);
    Mat score = outs[0];

    const int chns = score.size[1];
    const int rows = score.size[2];
    const int cols = score.size[3];

    mask.create(rows, cols, CV_8U);
    Mat classIds = mask.getMat();
    classIds.setTo(0);
    Mat maxVal(rows, cols, CV_32F, score.data);

    for (int ch = 1; ch < chns; ch++)
    {
        for (int row = 0; row < rows; row++)
        {
            const float *ptrScore = score.ptr<float>(0, ch, row);
            uint8_t *ptrMaxCl = classIds.ptr<uint8_t>(row);
            float *ptrMaxVal = maxVal.ptr<float>(row);
            for (int col = 0; col < cols; col++)
            {
                if (ptrScore[col] > ptrMaxVal[col])
                {
                    ptrMaxVal[col] = ptrScore[col];
                    ptrMaxCl[col] = ch;
                }
            }
        }
    }
}

class DetectionModel_Impl : public Model::Impl
{
public:
    virtual ~DetectionModel_Impl() {}
    DetectionModel_Impl() : Impl() {}
    DetectionModel_Impl(const DetectionModel_Impl&) = delete;
    DetectionModel_Impl(DetectionModel_Impl&&) = delete;

    void disableRegionNMS(Net& net)
    {
        for (String& name : net.getUnconnectedOutLayersNames())
        {
            int layerId = net.getLayerId(name);
            Ptr<RegionLayer> layer = net.getLayer(layerId).dynamicCast<RegionLayer>();
            if (!layer.empty())
            {
                layer->nmsThreshold = 0;
            }
        }
    }

    void setNmsAcrossClasses(bool value) {
        nmsAcrossClasses = value;
    }

    bool getNmsAcrossClasses() {
        return nmsAcrossClasses;
    }

private:
    bool nmsAcrossClasses = false;
};

DetectionModel::DetectionModel(const String& model, const String& config)
    : DetectionModel(readNet(model, config))
{
    // nothing
}

DetectionModel::DetectionModel(const Net& network) : Model()
{
    impl = makePtr<DetectionModel_Impl>();
    impl->initNet(network);
    impl.dynamicCast<DetectionModel_Impl>()->disableRegionNMS(getNetwork_());  // FIXIT Move to DetectionModel::Impl::initNet()
}

DetectionModel::DetectionModel() : Model()
{
    // nothing
}

DetectionModel& DetectionModel::setNmsAcrossClasses(bool value)
{
    CV_Assert(impl != nullptr && impl.dynamicCast<DetectionModel_Impl>() != nullptr); // remove once default constructor is removed

    impl.dynamicCast<DetectionModel_Impl>()->setNmsAcrossClasses(value);
    return *this;
}

bool DetectionModel::getNmsAcrossClasses()
{
    CV_Assert(impl != nullptr && impl.dynamicCast<DetectionModel_Impl>() != nullptr); // remove once default constructor is removed

    return impl.dynamicCast<DetectionModel_Impl>()->getNmsAcrossClasses();
}

void DetectionModel::detect(InputArray frame, CV_OUT std::vector<int>& classIds,
                            CV_OUT std::vector<float>& confidences, CV_OUT std::vector<Rect>& boxes,
                            float confThreshold, float nmsThreshold)
{
    CV_Assert(impl != nullptr && impl.dynamicCast<DetectionModel_Impl>() != nullptr); // remove once default constructor is removed

    std::vector<Mat> detections;
    impl->processFrame(frame, detections);

    boxes.clear();
    confidences.clear();
    classIds.clear();

    int frameWidth  = frame.cols();
    int frameHeight = frame.rows();
    if (getNetwork_().getLayer(0)->outputNameToIndex("im_info") != -1)
    {
        frameWidth = impl->size.width;
        frameHeight = impl->size.height;
    }

    std::vector<String> layerNames = getNetwork_().getLayerNames();
    int lastLayerId = getNetwork_().getLayerId(layerNames.back());
    Ptr<Layer> lastLayer = getNetwork_().getLayer(lastLayerId);

    if (lastLayer->type == "DetectionOutput")
    {
        // Network produces output blob with a shape 1x1xNx7 where N is a number of
        // detections and an every detection is a vector of values
        // [batchId, classId, confidence, left, top, right, bottom]
        for (int i = 0; i < detections.size(); ++i)
        {
            float* data = (float*)detections[i].data;
            for (int j = 0; j < detections[i].total(); j += 7)
            {
                float conf = data[j + 2];
                if (conf < confThreshold)
                    continue;

                int left   = data[j + 3];
                int top    = data[j + 4];
                int right  = data[j + 5];
                int bottom = data[j + 6];
                int width  = right  - left + 1;
                int height = bottom - top + 1;

                if (width <= 2 || height <= 2)
                {
                    left   = data[j + 3] * frameWidth;
                    top    = data[j + 4] * frameHeight;
                    right  = data[j + 5] * frameWidth;
                    bottom = data[j + 6] * frameHeight;
                    width  = right  - left + 1;
                    height = bottom - top + 1;
                }

                left   = std::max(0, std::min(left, frameWidth - 1));
                top    = std::max(0, std::min(top, frameHeight - 1));
                width  = std::max(1, std::min(width, frameWidth - left));
                height = std::max(1, std::min(height, frameHeight - top));
                boxes.emplace_back(left, top, width, height);

                classIds.push_back(static_cast<int>(data[j + 1]));
                confidences.push_back(conf);
            }
        }
    }
    else if (lastLayer->type == "Region")
    {
        std::vector<int> predClassIds;
        std::vector<Rect> predBoxes;
        std::vector<float> predConfidences;
        for (int i = 0; i < detections.size(); ++i)
        {
            // Network produces output blob with a shape NxC where N is a number of
            // detected objects and C is a number of classes + 4 where the first 4
            // numbers are [center_x, center_y, width, height]
            float* data = (float*)detections[i].data;
            for (int j = 0; j < detections[i].rows; ++j, data += detections[i].cols)
            {

                Mat scores = detections[i].row(j).colRange(5, detections[i].cols);
                Point classIdPoint;
                double conf;
                minMaxLoc(scores, nullptr, &conf, nullptr, &classIdPoint);

                if (static_cast<float>(conf) < confThreshold)
                    continue;

                int centerX = data[0] * frameWidth;
                int centerY = data[1] * frameHeight;
                int width   = data[2] * frameWidth;
                int height  = data[3] * frameHeight;

                int left = std::max(0, std::min(centerX - width / 2, frameWidth - 1));
                int top  = std::max(0, std::min(centerY - height / 2, frameHeight - 1));
                width    = std::max(1, std::min(width, frameWidth - left));
                height   = std::max(1, std::min(height, frameHeight - top));

                predClassIds.push_back(classIdPoint.x);
                predConfidences.push_back(static_cast<float>(conf));
                predBoxes.emplace_back(left, top, width, height);
            }
        }

        if (nmsThreshold)
        {
            if (getNmsAcrossClasses())
            {
                std::vector<int> indices;
                NMSBoxes(predBoxes, predConfidences, confThreshold, nmsThreshold, indices);
                for (int idx : indices)
                {
                    boxes.push_back(predBoxes[idx]);
                    confidences.push_back(predConfidences[idx]);
                    classIds.push_back(predClassIds[idx]);
                }
            }
            else
            {
                std::map<int, std::vector<size_t> > class2indices;
                for (size_t i = 0; i < predClassIds.size(); i++)
                {
                    if (predConfidences[i] >= confThreshold)
                    {
                        class2indices[predClassIds[i]].push_back(i);
                    }
                }
                for (const auto& it : class2indices)
                {
                    std::vector<Rect> localBoxes;
                    std::vector<float> localConfidences;
                    for (size_t idx : it.second)
                    {
                        localBoxes.push_back(predBoxes[idx]);
                        localConfidences.push_back(predConfidences[idx]);
                    }
                    std::vector<int> indices;
                    NMSBoxes(localBoxes, localConfidences, confThreshold, nmsThreshold, indices);
                    classIds.resize(classIds.size() + indices.size(), it.first);
                    for (int idx : indices)
                    {
                        boxes.push_back(localBoxes[idx]);
                        confidences.push_back(localConfidences[idx]);
                    }
                }
            }
        }
        else
        {
            boxes       = std::move(predBoxes);
            classIds    = std::move(predClassIds);
            confidences = std::move(predConfidences);
        }
    }
    else
        CV_Error(Error::StsNotImplemented, "Unknown output layer type: \"" + lastLayer->type + "\"");
}

}} // namespace
