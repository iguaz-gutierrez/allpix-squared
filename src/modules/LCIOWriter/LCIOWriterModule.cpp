/**
 * @file
 * @brief Implementation of [LCIOWriter] module
 * @copyright Copyright (c) 2017 CERN and the Allpix Squared authors.
 * This software is distributed under the terms of the MIT License, copied verbatim in the file "LICENSE.md".
 * In applying this license, CERN does not waive the privileges and immunities granted to it by virtue of its status as an
 * Intergovernmental Organization or submit itself to any jurisdiction.
 */

#include "LCIOWriterModule.hpp"

#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "core/messenger/Messenger.hpp"
#include "core/utils/file.h"
#include "core/utils/log.h"

#include <Math/RotationZYX.h>

#include <IMPL/LCCollectionVec.h>
#include <IMPL/LCEventImpl.h>
#include <IMPL/LCRunHeaderImpl.h>
#include <IMPL/TrackImpl.h>
#include <IMPL/TrackerDataImpl.h>
#include <IMPL/TrackerHitImpl.h>
#include <IMPL/TrackerPulseImpl.h>
#include <IO/LCWriter.h>
#include <IOIMPL/LCFactory.h>
#include <UTIL/CellIDEncoder.h>
#include <lcio.h>

using namespace allpix;
using namespace lcio;

LCIOWriterModule::LCIOWriterModule(Configuration& config, Messenger* messenger, GeometryManager* geo)
    : Module(config), geo_mgr_(geo) {

    // Bind pixel hits message
    messenger->bindMulti(this, &LCIOWriterModule::pixel_messages_, MsgFlags::REQUIRED);
    messenger->bindMulti(this, &LCIOWriterModule::mcparticle_messages_, MsgFlags::REQUIRED);
    messenger->bindSingle(this, &LCIOWriterModule::mctracks_message_, MsgFlags::REQUIRED);

    // Set configuration defaults:
    config_.setDefault("file_name", "output.slcio");
    config_.setDefault("geometry_file", "allpix_squared_gear.xml");
    config_.setDefault("pixel_type", 2);
    config_.setDefault("detector_name", "EUTelescope");
    config_.setDefault("output_collection_name", "zsdata_m26");
    config_.setDefault("dut_collection_name", "zsdata_dut");

    pixel_type_ = config_.get<int>("pixel_type");
    detector_name_ = config_.get<std::string>("detector_name");

    // The 'setup' parameter has a string matrix with three elements per row
    // ["detector_name", "output_collection", "sensor_id"] where the detector_name
    // must correspond to the detector name in the geometry file, the output_collection
    // will be the name of the lcio output collection (multiple detectors can write to
    // the same collection), and sensor_id has to be a unique id which the data corresponding
    // to this sensor will carry
    auto setup = config.getMatrix<std::string>("setup");
    auto assigned_ids = std::vector<unsigned>{};

    for(auto const& setup_entry : setup) {
        if(setup_entry.size() == 3) {
            auto const& det_name = setup_entry[0];
            auto const& col_name = setup_entry[1];
            auto const& sensor_id_str = setup_entry[2];
            // This map will help determine how many setup we will create (keys) and what
            // detectors write into that collection (values)
            col_to_dets_map_[col_name].emplace_back(det_name);

            unsigned sensor_id = 0;
            try {
                auto sensor_id_unchecked = std::stoi(sensor_id_str);
                if(sensor_id_unchecked >= 0 && sensor_id_unchecked <= 127) {
                    sensor_id = static_cast<unsigned>(sensor_id_unchecked);
                } else {
                    auto error = "The sensor id \"" + std::to_string(sensor_id_unchecked) +
                                 "\" which was provided for detector \"" + det_name +
                                 "\" must be positive and less than or equal to 127 (7 bit)";
                    throw InvalidValueError(config_, "setup", error);
                }
            } catch(const std::invalid_argument&) {
                auto error = "The sensor id \"" + sensor_id_str + "\" which was provided for detector \"" + det_name +
                             "\" is not a valid integer";
                throw InvalidValueError(config_, "setup", error);
            }

            if(std::find(assigned_ids.begin(), assigned_ids.end(), sensor_id) == assigned_ids.end()) {
                assigned_ids.emplace_back(sensor_id);
                // This map will translate the internally used detector name to the sensor id
                det_name_to_id_[det_name] = sensor_id;
            } else {
                auto error = "Trying to assign sensor id \"" + std::to_string(sensor_id) + "\" to detector \"" + det_name +
                             "\", this id is already assigned";
                throw InvalidValueError(config_, "setup", error);
            }

        } else {
            auto error = std::string("The entry: [");
            for(auto const& value : setup_entry) {
                error.append("\"" + value + "\", ");
            }
            error.pop_back();
            error.pop_back();
            error.append(
                "] should have three entries in following order: [\"detector_name\", \"output_collection\", \"sensor_id\"]");
            throw InvalidValueError(config_, "setup", error);
        }
    }

    //
    for(auto const& col_dets_pair : col_to_dets_map_) {
        col_name_vec_.emplace_back(col_dets_pair.first);
        LOG(DEBUG) << "Registered output collection \"" << col_dets_pair.first << "\" for sensors: ";
        for(auto const& det_name : col_dets_pair.second) {
            LOG(DEBUG) << det_name << " ";
            auto det_id = det_name_to_id_[det_name];
            det_id_to_col_index_[det_id] = col_name_vec_.size() - 1;
        }
    }

    // Cross check the detector geometry against the configuration file
    auto detectors = geo_mgr_->getDetectors();
    if(setup.size() != detectors.size()) {
        auto error = "In the configuration file " + std::to_string(setup.size()) +
                     " detectors are specified, in the geometry " + std::to_string(detectors.size()) +
                     ", this is a mismatch";
        throw InvalidValueError(config_, "setup", error);
    }
    for(const auto& det : detectors) {
        auto const& det_name = det->getName();
        auto it = det_name_to_id_.find(det_name);
        if(it != det_name_to_id_.end()) {
            LOG(DEBUG) << det_name << " has ID " << det_name_to_id_[det_name];
        } else {
            auto error = "Detector \"" + det_name +
                         "\" is specified in the geometry file, but not provided in the configuration file";
            throw InvalidValueError(config_, "setup", error);
        }
    }
}

void LCIOWriterModule::init() {
    // Create the output GEAR file for the detector geometry
    geometry_file_name_ = createOutputFile(allpix::add_file_extension(config_.get<std::string>("geometry_file"), "xml"));
    // Open LCIO file and write run header
    lcio_file_name_ = createOutputFile(allpix::add_file_extension(config_.get<std::string>("file_name"), "slcio"));
    lcWriter_ = std::shared_ptr<IO::LCWriter>(LCFactory::getInstance()->createLCWriter());
    lcWriter_->open(lcio_file_name_, LCIO::WRITE_NEW);
    auto run = std::make_unique<LCRunHeaderImpl>();
    run->setRunNumber(1);
    run->setDetectorName(detector_name_);
    lcWriter_->writeRunHeader(run.get());
}

void LCIOWriterModule::run(unsigned int eventNb) {

    auto evt = std::make_unique<LCEventImpl>(); // create the event
    evt->setRunNumber(1);
    evt->setEventNumber(static_cast<int>(eventNb)); // set the event attributes
    evt->parameters().setValue("EventType", 2);

    // The detector id is only attached to the message, not the MCParticle, thus we store it here
    auto mcp_to_det_id = std::map<MCParticle const*, unsigned>{};
    // Multiple pixel hits can be assigned to a single MCParticle, here we store them in a LCIO 'float vector' to create the
    // Monte Carlo truth cluster
    auto mcp_to_pixel_data_vec = std::map<MCParticle const*, std::vector<std::vector<float>>>{};
    // Every MCParticle will also be reflected by a TrackerData object
    auto mcp_to_tracker_data = std::map<MCParticle const*, TrackerDataImpl*>{};
    // Every track will be linked to at leat one (typically multiple) MCParticles and thus TrackerData objets
    auto mctrk_to_hit_data_vec = std::map<MCTrack const*, std::vector<TrackerHitImpl*>>{};

    auto output_col_vec = std::vector<LCCollectionVec*>();
    auto output_col_encoder_vec = std::vector<std::unique_ptr<CellIDEncoder<TrackerDataImpl>>>();
    // Prepare dynamic output setup and their CellIDEncoders which are defined by the user's config
    for(size_t i = 0; i < col_name_vec_.size(); ++i) {
        output_col_vec.emplace_back(new LCCollectionVec(LCIO::TRACKERDATA));
        std::cout << "Collection: " << output_col_vec.back() << std::endl;
        output_col_encoder_vec.emplace_back(
            std::make_unique<CellIDEncoder<TrackerDataImpl>>("sensorID:7,sparsePixelType:5", output_col_vec.back()));
    }

    // Prepare static Monte-Carlo output setup and their CellIDEncoders which are the same everytime
    LCCollectionVec* mc_cluster_vec = new LCCollectionVec(LCIO::TRACKERPULSE);
    LCCollectionVec* mc_cluster_raw_vec = new LCCollectionVec(LCIO::TRACKERDATA);
    LCCollectionVec* mc_hit_vec = new LCCollectionVec(LCIO::TRACKERHIT);
    LCCollectionVec* mc_track_vec = new LCCollectionVec(LCIO::TRACK);
    CellIDEncoder<TrackerDataImpl> mc_cluster_raw_encoder("sensorID:7,sparsePixelType:5", mc_cluster_raw_vec);
    CellIDEncoder<TrackerPulseImpl> mc_cluster_encoder("sensorID:7,xSeed:12,ySeed:12,xCluSize:5,yCluSize:5,type:5,quality:5",
                                                       mc_cluster_vec);
    CellIDEncoder<TrackerHitImpl> mc_hit_encoder("sensorID:7,properties:7", mc_hit_vec);

    // In LCIO the 'charge vector' is a vector of floats which correspond to hit pixels, depending on the pixel
    // type in EUTelescope the number of entries per pixel varies
    std::map<unsigned, std::vector<float>> charges;
    for(auto const& det : det_name_to_id_) {
        charges[det.second] = std::vector<float>{};
    }

    // Receive all pixel messages, fill charge vectors
    for(const auto& hit_msg : pixel_messages_) {
        LOG(DEBUG) << hit_msg->getDetector()->getName();
        for(const auto& hitdata : hit_msg->getData()) {

            auto thisHitCharge = std::vector<float>{};

            LOG(DEBUG) << "X: " << hitdata.getPixel().getIndex().x() << ", Y:" << hitdata.getPixel().getIndex().y()
                       << ", Signal: " << hitdata.getSignal();

            unsigned detectorID = det_name_to_id_[hit_msg->getDetector()->getName()];

            switch(pixel_type_) {
            case 1: // EUTelSimpleSparsePixel
                charges[detectorID].push_back(static_cast<float>(hitdata.getPixel().getIndex().x())); // x
                charges[detectorID].push_back(static_cast<float>(hitdata.getPixel().getIndex().y())); // y
                charges[detectorID].push_back(static_cast<float>(hitdata.getSignal()));               // signal
                thisHitCharge.push_back(static_cast<float>(hitdata.getPixel().getIndex().x()));       // x
                thisHitCharge.push_back(static_cast<float>(hitdata.getPixel().getIndex().y()));       // y
                thisHitCharge.push_back(static_cast<float>(hitdata.getSignal()));                     // signal
                break;
            case 2:  // EUTelGenericSparsePixel
            default: // EUTelGenericSparsePixel is default
                charges[detectorID].push_back(static_cast<float>(hitdata.getPixel().getIndex().x())); // x
                charges[detectorID].push_back(static_cast<float>(hitdata.getPixel().getIndex().y())); // y
                charges[detectorID].push_back(static_cast<float>(hitdata.getSignal()));               // signal
                charges[detectorID].push_back(0.0);
                thisHitCharge.push_back(static_cast<float>(hitdata.getPixel().getIndex().x())); // x
                thisHitCharge.push_back(static_cast<float>(hitdata.getPixel().getIndex().y())); // y
                thisHitCharge.push_back(static_cast<float>(hitdata.getSignal()));               // signal
                thisHitCharge.push_back(0.0);                                                   // time
                break;
            case 5: // EUTelTimepix3SparsePixel
                charges[detectorID].push_back(static_cast<float>(hitdata.getPixel().getIndex().x())); // x
                charges[detectorID].push_back(static_cast<float>(hitdata.getPixel().getIndex().y())); // y
                charges[detectorID].push_back(static_cast<float>(hitdata.getSignal()));               // signal
                charges[detectorID].push_back(0.0);                                                   // time
                charges[detectorID].push_back(0.0);                                                   // time
                charges[detectorID].push_back(0.0);                                                   // time
                charges[detectorID].push_back(0.0);                                                   // time
                thisHitCharge.push_back(static_cast<float>(hitdata.getPixel().getIndex().x()));       // x
                thisHitCharge.push_back(static_cast<float>(hitdata.getPixel().getIndex().y()));       // y
                thisHitCharge.push_back(static_cast<float>(hitdata.getSignal()));                     // signal
                thisHitCharge.push_back(0.0);                                                         // time
                thisHitCharge.push_back(0.0);                                                         // time
                thisHitCharge.push_back(0.0);                                                         // time
                thisHitCharge.push_back(0.0);                                                         // time
                break;
            }

            for(auto const& mcp : hitdata.getMCParticles()) {
                mcp_to_det_id[mcp] = detectorID;
                mcp_to_pixel_data_vec[mcp].emplace_back(thisHitCharge);
            }
        }
    }

    for(auto& pair : mcp_to_pixel_data_vec) {
        auto trackerData = new TrackerDataImpl();
        auto tracker_pulse = new TrackerPulseImpl();

        std::vector<float> finalChargeVec;
        for(auto const& signalVec : pair.second) {
            finalChargeVec.insert(std::end(finalChargeVec), std::begin(signalVec), std::end(signalVec));
        }

        trackerData->setChargeValues(finalChargeVec);
        mc_cluster_raw_encoder["sensorID"] = mcp_to_det_id[pair.first];
        mc_cluster_raw_encoder["sparsePixelType"] = pixel_type_;
        mc_cluster_raw_encoder.setCellID(trackerData);
        mcp_to_tracker_data[pair.first] = trackerData;
        mc_cluster_raw_vec->push_back(trackerData);

        tracker_pulse->setTrackerData(trackerData);
        mc_cluster_encoder["sensorID"] = mcp_to_det_id[pair.first];
        mc_cluster_encoder.setCellID(tracker_pulse);
        mc_cluster_vec->push_back(tracker_pulse);
    }

    // Fill hitvector with event data
    for(auto const& det_id_name_pair : det_name_to_id_) {
        auto det_id = det_id_name_pair.second;
        auto hit = new TrackerDataImpl();
        hit->setChargeValues(charges[det_id]);
        auto col_index = det_id_to_col_index_[det_id];
        output_col_encoder_vec[col_index]->operator[]("sensorID") = det_id;
        output_col_encoder_vec[col_index]->operator[]("sparsePixelType") = pixel_type_;
        output_col_encoder_vec[col_index]->setCellID(hit);
        output_col_vec[col_index]->push_back(hit);
    }

    for(const auto& mcparticle_msg : mcparticle_messages_) {
        for(const auto& mcp : mcparticle_msg->getData()) {
            // Every MCParticle will be reflected by a TrackerHitImpl
            auto hit = new TrackerHitImpl();
            auto detectorID = det_name_to_id_[mcparticle_msg->getDetector()->getName()];
            auto pos = mcp.getGlobalStartPoint();
            auto posArray = std::array<double, 3>{pos.x(), pos.y(), pos.z()};
            hit->setPosition(posArray.data());
            mc_hit_encoder["sensorID"] = detectorID;
            mc_hit_encoder.setCellID(hit);
            hit->rawHits() = std::vector<LCObject*>{mcp_to_tracker_data[&mcp]};
            mc_hit_vec->push_back(hit);
            mctrk_to_hit_data_vec[mcp.getTrack()].emplace_back(hit);
        }
    }

    LCFlagImpl flag(mc_track_vec->getFlag());
    flag.setBit(LCIO::TRBIT_HITS);
    mc_track_vec->setFlag(flag.getFlag());
    for(auto& pair : mctrk_to_hit_data_vec) {
        auto track = new TrackImpl();
        for(auto& hit : pair.second) {
            // std::cout << "Got hit: " << hit << " z-pos: " << hit->getPosition()[2] << '\n';
            track->addHit(hit);
        }
        mc_track_vec->push_back(track);
    }

    // Add collection to event and write event to LCIO file
    evt->addCollection(mc_track_vec, "mc_track");
    evt->addCollection(mc_hit_vec, "mc_hit");
    evt->addCollection(mc_cluster_raw_vec, "mc_raw_cluster");
    evt->addCollection(mc_cluster_vec, "mc_cluster");
    for(size_t i = 0; i < col_name_vec_.size(); i++) {
        evt->addCollection(output_col_vec[i], col_name_vec_[i]);
    }

    lcWriter_->writeEvent(evt.get()); // write the event to the file
    write_cnt_++;
}

void LCIOWriterModule::finalize() {
    lcWriter_->close();
    // Print statistics
    LOG(STATUS) << "Wrote " << write_cnt_ << " events to file:" << std::endl << lcio_file_name_;

    // Write geometry:
    std::ofstream geometry_file;
    if(!geometry_file_name_.empty()) {
        geometry_file.open(geometry_file_name_, std::ios_base::out | std::ios_base::trunc);
        if(!geometry_file.good()) {
            throw ModuleError("Cannot write to GEAR geometry file");
        }

        auto detectors = geo_mgr_->getDetectors();
        geometry_file << "<?xml version=\"1.0\" encoding=\"utf-8\"?>" << std::endl
                      << "<!-- ?xml-stylesheet type=\"text/xsl\" href=\"https://cern.ch/allpix-squared/\"? -->" << std::endl
                      << "<gear>" << std::endl;

        geometry_file << "  <global detectorName=\"" << detector_name_ << "\"/>" << std::endl;
        geometry_file << "  <detectors>" << std::endl;
        geometry_file << "    <detector name=\"SiPlanes\" geartype=\"SiPlanesParameters\">" << std::endl;
        geometry_file << "      <siplanesType type=\"TelescopeWithoutDUT\"/>" << std::endl;
        geometry_file << "      <siplanesNumber number=\"" << detectors.size() << "\"/>" << std::endl;
        geometry_file << "      <siplanesID ID=\"" << 0 << "\"/>" << std::endl;
        geometry_file << "      <layers>" << std::endl;

        for(auto& detector : detectors) {
            // Write header for the layer:
            geometry_file << "      <!-- Allpix Squared Detector: " << detector->getName()
                          << " - type: " << detector->getType() << " -->" << std::endl;
            geometry_file << "        <layer>" << std::endl;

            auto position = detector->getPosition();

            auto model = detector->getModel();
            auto npixels = model->getNPixels();
            auto pitch = model->getPixelSize();

            auto total_size = model->getSize();
            auto sensitive_size = model->getSensorSize();

            // Write ladder
            geometry_file << "          <ladder ID=\"" << det_name_to_id_[detector->getName()] << "\"" << std::endl;
            geometry_file << "            positionX=\"" << Units::convert(position.x(), "mm") << "\"\tpositionY=\""
                          << Units::convert(position.y(), "mm") << "\"\tpositionZ=\"" << Units::convert(position.z(), "mm")
                          << "\"" << std::endl;

            // Use inverse ZYX rotation to retrieve XYZ angles as used in EUTelescope:
            ROOT::Math::RotationZYX rotations(detector->getOrientation().Inverse());
            geometry_file << "            rotationZY=\"" << Units::convert(-rotations.Psi(), "deg") << "\"     rotationZX=\""
                          << Units::convert(-rotations.Theta(), "deg") << "\"   rotationXY=\""
                          << Units::convert(-rotations.Phi(), "deg") << "\"" << std::endl;
            geometry_file << "            sizeX=\"" << Units::convert(total_size.x(), "mm") << "\"\tsizeY=\""
                          << Units::convert(total_size.y(), "mm") << "\"\tthickness=\""
                          << Units::convert(total_size.z(), "mm") << "\"" << std::endl;
            geometry_file << "            radLength=\"93.65\"" << std::endl;
            geometry_file << "            />" << std::endl;

            // Write sensitive
            geometry_file << "          <sensitive ID=\"" << det_name_to_id_[detector->getName()] << "\"" << std::endl;
            geometry_file << "            positionX=\"" << Units::convert(position.x(), "mm") << "\"\tpositionY=\""
                          << Units::convert(position.y(), "mm") << "\"\tpositionZ=\"" << Units::convert(position.z(), "mm")
                          << "\"" << std::endl;
            geometry_file << "            sizeX=\"" << Units::convert(npixels.x() * pitch.x(), "mm") << "\"\tsizeY=\""
                          << Units::convert(npixels.y() * pitch.y(), "mm") << "\"\tthickness=\""
                          << Units::convert(sensitive_size.z(), "mm") << "\"" << std::endl;
            geometry_file << "            npixelX=\"" << npixels.x() << "\"\tnpixelY=\"" << npixels.y() << "\"" << std::endl;
            geometry_file << "            pitchX=\"" << Units::convert(pitch.x(), "mm") << "\"\tpitchY=\""
                          << Units::convert(pitch.y(), "mm") << "\"\tresolution=\""
                          << Units::convert(pitch.x() / std::sqrt(12), "mm") << "\"" << std::endl;
            geometry_file << "            rotation1=\"1.0\"\trotation2=\"0.0\"" << std::endl;
            geometry_file << "            rotation3=\"0.0\"\trotation4=\"1.0\"" << std::endl;
            geometry_file << "            radLength=\"93.65\"" << std::endl;
            geometry_file << "            />" << std::endl;

            // End the layer:
            geometry_file << "        </layer>" << std::endl;
        }

        // Close XML tree:
        geometry_file << "      </layers>" << std::endl
                      << "    </detector>" << std::endl
                      << "  </detectors>" << std::endl
                      << "</gear>" << std::endl;

        LOG(STATUS) << "Wrote GEAR geometry to file:" << std::endl << geometry_file_name_;
    }
}
