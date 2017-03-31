/**
 * @author Koen Wolters <koen.wolters@cern.ch>
 */

#include <iomanip>

#include "TestDepositReaderModule.hpp"

#include "core/messenger/Messenger.hpp"
#include "core/utils/log.h"

#include "core/utils/unit.h"
#include "tools/ROOT.h"

using namespace allpix;

const std::string TestDepositReaderModule::name = "deposit_reader_test";

TestDepositReaderModule::TestDepositReaderModule(Configuration config, Messenger* messenger, GeometryManager*)
    : config_(std::move(config)), deposit_messages_() {
    // fetch deposits
    messenger->bindMulti(this, &TestDepositReaderModule::deposit_messages_);
}
TestDepositReaderModule::~TestDepositReaderModule() = default;

// print the deposits
void TestDepositReaderModule::run() {
    LOG(INFO) << "Got deposits in " << deposit_messages_.size() << " detectors";
    for(auto& message : deposit_messages_) {
        LOG(DEBUG) << "set of " << message->getData().size() << " deposits in detector "
                   << message->getDetector()->getName();
        for(auto& deposit : message->getData()) {
            auto pos = deposit.getPosition();

            auto x = Units::convert(pos.x(), "um");
            auto y = Units::convert(pos.y(), "um");
            auto z = Units::convert(pos.z(), "um");

            LOG(DEBUG) << " " << std::fixed << std::setprecision(5) << deposit.getCharge()
                       << " charges deposited at position (" << x << "um," << y << "um," << z << "um)";
        }
    }
    deposit_messages_.clear();
}

// External function, to allow loading from dynamic library without knowing module type.
// Should be overloaded in all module implementations, added here to prevent crashes
Module* allpix::generator(Configuration config, Messenger* messenger, GeometryManager* geometry) {
    TestDepositReaderModule* module = new TestDepositReaderModule(config, messenger, geometry);
    return dynamic_cast<Module*>(module);
}
