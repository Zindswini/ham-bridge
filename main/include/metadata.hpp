// Standard Library
#include <cstdint>
#include <string>

// ESP-IDF

// External Components
#include "enums/features.pb.h"
#include "messages/hello.pb.h"
#include "nanopb_helper.hpp"

// Local Includes

// Server Metadata
static constexpr uint32_t PROTOCOL_VERSION = 1;
static constexpr std::string FIRMWARE_VERSION = "0.0.0dev";

static constexpr std::array<Zindswini_ham_bridge_Feature, 3> kFeatureList = {
    Zindswini_ham_bridge_Feature_FEATURE_AUDIO_PCM, Zindswini_ham_bridge_Feature_FEATURE_PRECOMPUTED_SPECTRUM,
    Zindswini_ham_bridge_Feature_FEATURE_TX};

static constexpr Zindswini_ham_bridge_ServerInfo kServerInfo = {
    .server_firmware_version = npb::str("0.0.0"),
    .server_nickname = npb::str("dev_board"),
    .radio_model = Zindswini_ham_bridge_RadioModel_RADIO_MODEL_XIEGU_G90,
    .features = npb::enum_list(&kFeatureList)};