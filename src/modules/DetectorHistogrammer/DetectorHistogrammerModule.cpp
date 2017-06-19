/**
 * @author Koen Wolters <koen.wolters@cern.ch>
 */

#include "DetectorHistogrammerModule.hpp"

#include <memory>
#include <string>
#include <utility>

#include "core/geometry/PixelDetectorModel.hpp"
#include "core/messenger/Messenger.hpp"
#include "core/utils/log.h"

#include "tools/ROOT.h"

using namespace allpix;

DetectorHistogrammerModule::DetectorHistogrammerModule(Configuration config,
                                                       Messenger* messenger,
                                                       std::shared_ptr<Detector> detector)
    : Module(config, detector), config_(std::move(config)), detector_(std::move(detector)), pixels_message_(nullptr) {
    // fetch deposit for single module
    messenger->bindSingle(this, &DetectorHistogrammerModule::pixels_message_, MsgFlags::REQUIRED);
}

// create histograms
void DetectorHistogrammerModule::init() {
    // get detector model
    auto model = std::dynamic_pointer_cast<PixelDetectorModel>(detector_->getModel());
    if(model == nullptr) {
        throw ModuleError("Detector model of " + detector_->getName() +
                          " is not a PixelDetectorModel: other models are not supported by this module!");
    }

    // create histogram
    LOG(TRACE) << "Creating histograms";
    std::string histogram_name = "histogram";
    std::string histogram_title = "Hitmap for " + detector_->getName() + ";x (pixels);y (pixels)";
    histogram = new TH2I(histogram_name.c_str(),
                         histogram_title.c_str(),
                         model->getNPixelsX(),
                         -0.5,
                         model->getNPixelsX() - 0.5,
                         model->getNPixelsY(),
                         -0.5,
                         model->getNPixelsY() - 0.5);

    // create cluster size plot
    std::string cluster_size_name = "cluster";
    std::string cluster_size_title = "Cluster size for " + detector_->getName() + ";size;number";
    cluster_size = new TH1I(cluster_size_name.c_str(),
                            cluster_size_title.c_str(),
                            model->getNPixelsX() * model->getNPixelsY(),
                            0.5,
                            model->getNPixelsX() * model->getNPixelsY() + 0.5);
}

// fill the histograms
void DetectorHistogrammerModule::run(unsigned int) {
    LOG(DEBUG) << "Adding hits in " << pixels_message_->getData().size() << " pixels";

    // fill 2d histogram
    for(auto& pixel_charge : pixels_message_->getData()) {
        auto pixel = pixel_charge.getPixel();

        // Add pixel
        histogram->Fill(pixel.x(), pixel.y());

        // Update statistics
        total_vector_ += pixel;
        total_hits_ += 1;
    }

    // fill cluster histogram
    cluster_size->Fill(static_cast<double>(pixels_message_->getData().size()));
}

// create file and write the histograms to it
void DetectorHistogrammerModule::finalize() {
    LOG(INFO) << "Plotted " << total_hits_ << " hits in total, mean position is "
              << display_vector(total_vector_ / static_cast<double>(total_hits_), {"mm", "um"});

    // set more useful spacing maximum for cluster size histogram
    auto xmax = std::ceil(cluster_size->GetBinCenter(cluster_size->FindLastBinAbove()) + 1);
    cluster_size->GetXaxis()->SetRangeUser(0, xmax);
    // set cluster size axis spacing (FIXME: worth to do?)
    if(static_cast<int>(xmax) < 10) {
        cluster_size->GetXaxis()->SetNdivisions(static_cast<int>(xmax) + 1, 0, 0, true);
    }

    // set default drawing option histogram
    histogram->SetOption("colz");
    // set histogram axis spacing (FIXME: worth to do?)
    if(static_cast<int>(histogram->GetXaxis()->GetXmax()) < 10) {
        histogram->GetXaxis()->SetNdivisions(static_cast<int>(histogram->GetXaxis()->GetXmax()) + 1, 0, 0, true);
    }
    if(static_cast<int>(histogram->GetYaxis()->GetXmax()) < 10) {
        histogram->GetYaxis()->SetNdivisions(static_cast<int>(histogram->GetYaxis()->GetXmax()) + 1, 0, 0, true);
    }

    // write histograms
    LOG(TRACE) << "Writing histograms to file";
    histogram->Write();
    cluster_size->Write();
}