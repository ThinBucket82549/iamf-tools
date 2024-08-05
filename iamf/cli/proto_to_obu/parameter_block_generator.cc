/*
 * Copyright (c) 2023, Alliance for Open Media. All rights reserved
 *
 * This source code is subject to the terms of the BSD 3-Clause Clear License
 * and the Alliance for Open Media Patent License 1.0. If the BSD 3-Clause Clear
 * License was not distributed with this source code in the LICENSE file, you
 * can obtain it at www.aomedia.org/license/software-license/bsd-3-c-c. If the
 * Alliance for Open Media Patent License 1.0 was not distributed with this
 * source code in the PATENTS file, you can obtain it at
 * www.aomedia.org/license/patent.
 */
#include "iamf/cli/proto_to_obu/parameter_block_generator.h"

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "iamf/cli/audio_element_with_data.h"
#include "iamf/cli/channel_label.h"
#include "iamf/cli/cli_util.h"
#include "iamf/cli/demixing_module.h"
#include "iamf/cli/global_timing_module.h"
#include "iamf/cli/parameter_block_with_data.h"
#include "iamf/cli/proto/parameter_block.pb.h"
#include "iamf/cli/proto/parameter_data.pb.h"
#include "iamf/cli/recon_gain_generator.h"
#include "iamf/common/macros.h"
#include "iamf/common/obu_util.h"
#include "iamf/obu/audio_element.h"
#include "iamf/obu/demixing_info_param_data.h"
#include "iamf/obu/leb128.h"
#include "iamf/obu/param_definitions.h"
#include "iamf/obu/parameter_block.h"

namespace iamf_tools {

namespace {

absl::Status GetPerIdMetadata(
    const DecodedUleb128 parameter_id,
    const absl::flat_hash_map<DecodedUleb128, AudioElementWithData>&
        audio_elements,
    const ParamDefinition* param_definition,
    PerIdParameterMetadata& per_id_metadata) {
  RETURN_IF_NOT_OK(ValidateHasValue(param_definition->GetType(),
                                    "`param_definition_type`."));
  // Initialize some fields that may not be set later.
  per_id_metadata = PerIdParameterMetadata{
      .param_definition_type = param_definition->GetType().value(),
      .param_definition = *param_definition,
      .num_layers = 0,
  };
  if (per_id_metadata.param_definition_type ==
      ParamDefinition::kParameterDefinitionReconGain) {
    const ReconGainParamDefinition* recon_gain_param_definition =
        static_cast<const ReconGainParamDefinition*>(param_definition);

    auto audio_element_iter =
        audio_elements.find(recon_gain_param_definition->audio_element_id_);
    if (audio_element_iter == audio_elements.end()) {
      return absl::InvalidArgumentError(absl::StrCat(
          "Audio Element ID: ", recon_gain_param_definition->audio_element_id_,
          " associated with the recon gain parameter of ID: ", parameter_id,
          " not found"));
    }

    per_id_metadata.audio_element_id = audio_element_iter->first;
    const auto& channel_config = std::get<ScalableChannelLayoutConfig>(
        audio_element_iter->second.obu.config_);
    per_id_metadata.num_layers = channel_config.num_layers;
    per_id_metadata.recon_gain_is_present_flags.resize(
        per_id_metadata.num_layers);
    for (int l = 0; l < per_id_metadata.num_layers; l++) {
      per_id_metadata.recon_gain_is_present_flags[l] =
          (channel_config.channel_audio_layer_configs[l]
               .recon_gain_is_present_flag == 1);
    }
    per_id_metadata.channel_numbers_for_layers =
        audio_element_iter->second.channel_numbers_for_layers;
  }
  return absl::OkStatus();
}

absl::Status GenerateMixGainSubblock(
    const iamf_tools_cli_proto::MixGainParameterData&
        metadata_mix_gain_parameter_data,
    MixGainParameterData& obu_mix_gain_param_data) {
  switch (metadata_mix_gain_parameter_data.animation_type()) {
    using enum iamf_tools_cli_proto::AnimationType;
    case ANIMATE_STEP: {
      const auto& metadata_animation =
          metadata_mix_gain_parameter_data.param_data().step();
      obu_mix_gain_param_data.animation_type =
          MixGainParameterData::kAnimateStep;
      AnimationStepInt16 obu_animation;
      RETURN_IF_NOT_OK(Int32ToInt16(metadata_animation.start_point_value(),
                                    obu_animation.start_point_value));
      obu_mix_gain_param_data.param_data = obu_animation;
      break;
    }
    case ANIMATE_LINEAR: {
      const auto& metadata_animation =
          metadata_mix_gain_parameter_data.param_data().linear();
      obu_mix_gain_param_data.animation_type =
          MixGainParameterData::kAnimateLinear;

      AnimationLinearInt16 obu_animation;
      RETURN_IF_NOT_OK(Int32ToInt16(metadata_animation.start_point_value(),
                                    obu_animation.start_point_value));
      RETURN_IF_NOT_OK(Int32ToInt16(metadata_animation.end_point_value(),
                                    obu_animation.end_point_value));
      obu_mix_gain_param_data.param_data = obu_animation;
      break;
    }
    case ANIMATE_BEZIER: {
      const auto& metadata_animation =
          metadata_mix_gain_parameter_data.param_data().bezier();
      obu_mix_gain_param_data.animation_type =
          MixGainParameterData::kAnimateBezier;
      AnimationBezierInt16 obu_animation;
      RETURN_IF_NOT_OK(Int32ToInt16(metadata_animation.start_point_value(),
                                    obu_animation.start_point_value));
      RETURN_IF_NOT_OK(Int32ToInt16(metadata_animation.end_point_value(),
                                    obu_animation.end_point_value));
      RETURN_IF_NOT_OK(Int32ToInt16(metadata_animation.control_point_value(),
                                    obu_animation.control_point_value));
      RETURN_IF_NOT_OK(
          Uint32ToUint8(metadata_animation.control_point_relative_time(),
                        obu_animation.control_point_relative_time));
      obu_mix_gain_param_data.param_data = obu_animation;
      break;
    }
    default:
      return absl::InvalidArgumentError(
          absl::StrCat("Unrecognized animation type= ",
                       metadata_mix_gain_parameter_data.animation_type()));
  }

  return absl::OkStatus();
}

absl::Status FindDemixedChannels(
    const ChannelNumbers& accumulated_channels,
    const ChannelNumbers& layer_channels,
    std::list<ChannelLabel::Label>* const demixed_channel_labels) {
  using enum ChannelLabel::Label;
  for (int surround = accumulated_channels.surround + 1;
       surround <= layer_channels.surround; surround++) {
    switch (surround) {
      case 2:
        // Previous layer is Mono, this layer is Stereo.
        if (accumulated_channels.surround == 1) {
          demixed_channel_labels->push_back(kDemixedR2);
        }
        break;
      case 3:
        demixed_channel_labels->push_back(kDemixedL3);
        demixed_channel_labels->push_back(kDemixedR3);
        break;
      case 5:
        demixed_channel_labels->push_back(kDemixedLs5);
        demixed_channel_labels->push_back(kDemixedRs5);
        break;
      case 7:
        demixed_channel_labels->push_back(kDemixedL7);
        demixed_channel_labels->push_back(kDemixedR7);
        demixed_channel_labels->push_back(kDemixedLrs7);
        demixed_channel_labels->push_back(kDemixedRrs7);
        break;
      default:
        if (surround > 7) {
          return absl::InvalidArgumentError(absl::StrCat(
              "Unsupported number of surround channels: ", surround));
        }
        break;
    }
  }

  if (accumulated_channels.height == 2) {
    if (layer_channels.height == 4) {
      demixed_channel_labels->push_back(kDemixedLtb4);
      demixed_channel_labels->push_back(kDemixedRtb4);
    } else if (layer_channels.height == 2 &&
               accumulated_channels.surround == 3 &&
               layer_channels.surround > 3) {
      demixed_channel_labels->push_back(kDemixedLtf2);
      demixed_channel_labels->push_back(kDemixedRtf2);
    }
  }

  return absl::OkStatus();
}

absl::Status ConvertReconGainsAndFlags(
    const bool additional_logging,
    const absl::flat_hash_map<ChannelLabel::Label, double>& label_to_recon_gain,
    std::vector<uint8_t>& computed_recon_gains,
    DecodedUleb128& computed_recon_gain_flag) {
  computed_recon_gains.resize(12, 0);
  computed_recon_gain_flag = 0;
  for (const auto& [label, recon_gain] : label_to_recon_gain) {
    LOG_IF(INFO, additional_logging)
        << "Recon Gain[" << label << "]= " << recon_gain;

    // Bit position is based on Figure 5 of the Spec.
    int bit_position = 0;
    switch (label) {
      using enum ChannelLabel::Label;
      case kDemixedL7:
      case kDemixedL5:
      case kDemixedL3:
        // `kDemixedL2` is never demixed.
        bit_position = 0;
        break;
      case kDemixedR7:
      case kDemixedR5:
      case kDemixedR3:
      case kDemixedR2:
        // `kCentre` is never demixed. Skipping bit position = 1.
        bit_position = 2;
        break;
      case kDemixedLs5:
        bit_position = 3;
        break;
      case kDemixedRs5:
        bit_position = 4;
        break;
      case kDemixedLtf2:
        bit_position = 5;
        break;
      case kDemixedRtf2:
        bit_position = 6;
        break;
      case kDemixedLrs7:
        bit_position = 7;
        break;
      case kDemixedRrs7:
        bit_position = 8;
        break;
      case kDemixedLtb4:
        bit_position = 9;
        break;
      case kDemixedRtb4:
        bit_position = 10;
        // `kLFE` is never demixed. Skipping bit position = 11.
        break;
      default:
        LOG(ERROR) << "Unrecognized demixed channel label: " << label;
    }
    computed_recon_gain_flag |= 1 << bit_position;
    computed_recon_gains[bit_position] =
        static_cast<uint8_t>(recon_gain * 255.0);
  }
  return absl::OkStatus();
}

absl::Status ComputeReconGains(
    const int layer_index, const ChannelNumbers& layer_channels,
    const ChannelNumbers& accumulated_channels,
    const bool additional_recon_gains_logging,
    const LabelSamplesMap& labeled_samples,
    const LabelSamplesMap& label_to_decoded_samples,
    const std::vector<bool>& recon_gain_is_present_flags,
    std::vector<uint8_t>& computed_recon_gains,
    DecodedUleb128& computed_recon_gain_flag) {
  if (additional_recon_gains_logging) {
    LogChannelNumbers(absl::StrCat("Layer[", layer_index, "]"), layer_channels);
  }
  absl::flat_hash_map<ChannelLabel::Label, double> label_to_recon_gain;
  if (layer_index > 0) {
    std::list<ChannelLabel::Label> demixed_channel_labels;
    RETURN_IF_NOT_OK(FindDemixedChannels(accumulated_channels, layer_channels,
                                         &demixed_channel_labels));

    LOG_IF(INFO, additional_recon_gains_logging) << "Demixed channels: ";
    for (const auto& label : demixed_channel_labels) {
      RETURN_IF_NOT_OK(ReconGainGenerator::ComputeReconGain(
          label, labeled_samples, label_to_decoded_samples,
          additional_recon_gains_logging, label_to_recon_gain[label]));
    }
  }

  if (recon_gain_is_present_flags[layer_index] !=
      (!label_to_recon_gain.empty())) {
    return absl::InvalidArgumentError(absl::StrCat(
        "Mismatch of whether user specified recon gain is present: ",
        recon_gain_is_present_flags[layer_index],
        " vs whether recon gain should be computed: ",
        !label_to_recon_gain.empty()));
  }

  RETURN_IF_NOT_OK(ConvertReconGainsAndFlags(
      /*additional_logging=*/true, label_to_recon_gain, computed_recon_gains,
      computed_recon_gain_flag));

  return absl::OkStatus();
}

absl::Status GenerateReconGainSubblock(
    const bool override_computed_recon_gains,
    const bool additional_recon_gains_logging,
    const IdLabeledFrameMap& id_to_labeled_frame,
    const IdLabeledFrameMap& id_to_labeled_decoded_frame,
    const uint8_t num_layers,
    const std::vector<bool>& recon_gain_is_present_flags,
    const std::vector<ChannelNumbers>& channel_numbers_for_layers,
    const iamf_tools_cli_proto::ReconGainInfoParameterData&
        metadata_recon_gain_info_parameter_data,
    const DecodedUleb128 audio_element_id,
    ReconGainInfoParameterData& obu_recon_gain_info_param_data) {
  const auto& user_recon_gains_layers =
      metadata_recon_gain_info_parameter_data.recon_gains_for_layer();
  if (num_layers > 1 &&
      static_cast<size_t>(num_layers) != user_recon_gains_layers.size()) {
    return absl::InvalidArgumentError(
        absl::StrCat("There are ", num_layers, " layers of scalable  ",
                     "audio element, but the user only specifies ",
                     user_recon_gains_layers.size(), " layers."));
  }
  obu_recon_gain_info_param_data.recon_gain_elements.resize(num_layers);

  ChannelNumbers accumulated_channels = {0, 0, 0};
  for (int layer_index = 0; layer_index < num_layers; layer_index++) {
    // Construct the bitmask indicating the channels where recon gains are
    // present.
    std::vector<uint8_t> user_recon_gains(12, 0);
    DecodedUleb128 user_recon_gain_flag = 0;
    for (const auto& [bit_position, user_recon_gain] :
         user_recon_gains_layers[layer_index].recon_gain()) {
      user_recon_gain_flag |= 1 << bit_position;
      user_recon_gains[bit_position] = user_recon_gain;
    }

    // Write out the user supplied gains. Depending on the mode these either
    // match the computed recon gains or are used as an override. Write to
    // output.
    auto& output_recon_gain_element =
        obu_recon_gain_info_param_data.recon_gain_elements[layer_index];
    for (const auto& [bit_position, user_recon_gain] :
         user_recon_gains_layers[layer_index].recon_gain()) {
      output_recon_gain_element.recon_gain[bit_position] =
          user_recon_gains[bit_position];
    }
    output_recon_gain_element.recon_gain_flag = user_recon_gain_flag;

    if (override_computed_recon_gains) {
      continue;
    }

    // Compute the recon gains and validate they match the user supplied values.
    const auto& layer_channels = channel_numbers_for_layers[layer_index];
    std::vector<uint8_t> computed_recon_gains;
    DecodedUleb128 computed_recon_gain_flag = 0;

    const auto labeled_frame_iter = id_to_labeled_frame.find(audio_element_id);
    const auto labeled_decoded_frame_iter =
        id_to_labeled_decoded_frame.find(audio_element_id);
    if (labeled_frame_iter == id_to_labeled_frame.end() ||
        labeled_decoded_frame_iter == id_to_labeled_decoded_frame.end()) {
      return absl::InvalidArgumentError(absl::StrCat(
          "Original or decoded audio frame for audio element ID= ",
          audio_element_id, " not found when computing recon gains"));
    }

    RETURN_IF_NOT_OK(
        ComputeReconGains(layer_index, layer_channels, accumulated_channels,
                          additional_recon_gains_logging,
                          labeled_frame_iter->second.label_to_samples,
                          labeled_decoded_frame_iter->second.label_to_samples,
                          recon_gain_is_present_flags, computed_recon_gains,
                          computed_recon_gain_flag));
    accumulated_channels = layer_channels;

    if (!recon_gain_is_present_flags[layer_index]) {
      continue;
    }

    // Compare computed and user specified flag and recon gain values.
    if (computed_recon_gain_flag != user_recon_gain_flag) {
      return absl::InvalidArgumentError(absl::StrCat(
          "Computed recon gain flag different from what user specified: ",
          computed_recon_gain_flag, " vs ", user_recon_gain_flag));
    }
    bool recon_gains_match = true;
    for (int i = 0; i < 12; i++) {
      if (user_recon_gains[i] != computed_recon_gains[i]) {
        // Find all mismatches before returning an error.
        LOG(ERROR) << "Computed recon gain [" << i
                   << "] different from what user specified: "
                   << absl::StrCat(computed_recon_gains[i]) << " vs "
                   << absl::StrCat(user_recon_gains[i]);
        recon_gains_match = false;
      }
    }
    if (!recon_gains_match) {
      return absl::InvalidArgumentError("Recon gains mismatch");
    }
  }  // End of for (int layer_index ...)

  return absl::OkStatus();
}

absl::Status GenerateParameterBlockSubblock(
    const bool override_computed_recon_gains,
    const bool additional_recon_gains_logging,
    const IdLabeledFrameMap* id_to_labeled_frame,
    const IdLabeledFrameMap* id_to_labeled_decoded_frame,
    const PerIdParameterMetadata& per_id_metadata,
    const bool include_subblock_duration, const int subblock_index,
    const iamf_tools_cli_proto::ParameterSubblock& metadata_subblock,
    ParameterBlockObu& obu) {
  if (include_subblock_duration) {
    RETURN_IF_NOT_OK(obu.SetSubblockDuration(
        subblock_index, metadata_subblock.subblock_duration()));
  }
  ParameterSubblock& obu_subblock = obu.subblocks_[subblock_index];

  switch (per_id_metadata.param_definition_type) {
    using enum ParamDefinition::ParameterDefinitionType;
    case kParameterDefinitionMixGain: {
      MixGainParameterData param_data;
      RETURN_IF_NOT_OK(GenerateMixGainSubblock(
          metadata_subblock.mix_gain_parameter_data(), param_data));
      obu_subblock.param_data = param_data;
      break;
    }
    case kParameterDefinitionDemixing: {
      if (subblock_index > 1) {
        return absl::InvalidArgumentError(
            "There should be only one subblock for demixing info.");
      }
      DemixingInfoParameterData param_data;
      RETURN_IF_NOT_OK(CopyDemixingInfoParameterData(
          metadata_subblock.demixing_info_parameter_data(), param_data));
      obu_subblock.param_data = param_data;
      break;
    }
    case kParameterDefinitionReconGain: {
      if (subblock_index > 1) {
        return absl::InvalidArgumentError(
            "There should be only one subblock for recon gain info.");
      }
      ReconGainInfoParameterData param_data;
      RETURN_IF_NOT_OK(GenerateReconGainSubblock(
          override_computed_recon_gains, additional_recon_gains_logging,
          *id_to_labeled_frame, *id_to_labeled_decoded_frame,
          per_id_metadata.num_layers,
          per_id_metadata.recon_gain_is_present_flags,
          per_id_metadata.channel_numbers_for_layers,
          metadata_subblock.recon_gain_info_parameter_data(),
          per_id_metadata.audio_element_id, param_data));
      obu_subblock.param_data = param_data;
      break;
    }
    default:
      // TODO(b/289080630): Support the extension fields here.
      return absl::InvalidArgumentError(
          absl::StrCat("Unsupported param definition type= ",
                       per_id_metadata.param_definition_type));
  }

  return absl::OkStatus();
}

absl::Status PopulateCommonFields(
    const iamf_tools_cli_proto::ParameterBlockObuMetadata&
        parameter_block_metadata,
    PerIdParameterMetadata& per_id_metadata,
    GlobalTimingModule& global_timing_module,
    ParameterBlockWithData& parameter_block_with_data) {
  // Get the duration from the parameter definition or the OBU itself as
  // applicable.
  const DecodedUleb128 duration =
      per_id_metadata.param_definition.param_definition_mode_ == 1
          ? parameter_block_metadata.duration()
          : per_id_metadata.param_definition.duration_;

  // Populate the timing information.
  RETURN_IF_NOT_OK(global_timing_module.GetNextParameterBlockTimestamps(
      parameter_block_metadata.parameter_id(),
      parameter_block_metadata.start_timestamp(), duration,
      parameter_block_with_data.start_timestamp,
      parameter_block_with_data.end_timestamp));

  // Populate the OBU.
  const DecodedUleb128 parameter_id = parameter_block_metadata.parameter_id();
  parameter_block_with_data.obu = std::make_unique<ParameterBlockObu>(
      GetHeaderFromMetadata(parameter_block_metadata.obu_header()),
      parameter_id, per_id_metadata);

  // Several fields are dependent on `param_definition_mode`.
  if (per_id_metadata.param_definition.param_definition_mode_ == 1) {
    RETURN_IF_NOT_OK(parameter_block_with_data.obu->InitializeSubblocks(
        parameter_block_metadata.duration(),
        parameter_block_metadata.constant_subblock_duration(),
        parameter_block_metadata.num_subblocks()));
  } else {
    RETURN_IF_NOT_OK(parameter_block_with_data.obu->InitializeSubblocks());
  }

  return absl::OkStatus();
}

absl::Status PopulateSubblocks(
    const iamf_tools_cli_proto::ParameterBlockObuMetadata&
        parameter_block_metadata,
    const bool override_computed_recon_gains,
    const bool additional_recon_gains_logging,
    const IdLabeledFrameMap* id_to_labeled_frame,
    const IdLabeledFrameMap* id_to_labeled_decoded_frame,
    PerIdParameterMetadata& per_id_metadata,
    ParameterBlockWithData& output_parameter_block) {
  auto& parameter_block_obu = *output_parameter_block.obu;
  const DecodedUleb128 num_subblocks = parameter_block_obu.GetNumSubblocks();

  // All subblocks will include `subblock_duration` or none will include it.
  const bool include_subblock_duration =
      per_id_metadata.param_definition.param_definition_mode_ == 1 &&
      parameter_block_obu.GetConstantSubblockDuration() == 0;

  if (num_subblocks != parameter_block_metadata.subblocks_size()) {
    return absl::InvalidArgumentError(
        absl::StrCat("Expected ", num_subblocks, " subblocks, got ",
                     parameter_block_metadata.subblocks_size()));
  }
  for (int i = 0; i < num_subblocks; ++i) {
    RETURN_IF_NOT_OK(GenerateParameterBlockSubblock(
        override_computed_recon_gains, additional_recon_gains_logging,
        id_to_labeled_frame, id_to_labeled_decoded_frame, per_id_metadata,
        include_subblock_duration, i, parameter_block_metadata.subblocks(i),
        parameter_block_obu));
  }

  return absl::OkStatus();
}

absl::Status LogParameterBlockObus(
    const std::list<ParameterBlockWithData>& output_parameter_blocks) {
  // Log only the first and the last parameter blocks.
  if (output_parameter_blocks.empty()) {
    return absl::OkStatus();
  }
  std::vector<const ParameterBlockWithData*> to_log = {
      &output_parameter_blocks.front()};
  if (output_parameter_blocks.size() > 1) {
    to_log.push_back(&output_parameter_blocks.back());
  }

  for (const auto* parameter_block_with_data : to_log) {
    parameter_block_with_data->obu->PrintObu();
    LOG(INFO) << "  // start_timestamp= "
              << parameter_block_with_data->start_timestamp;
    LOG(INFO) << "  // end_timestamp= "
              << parameter_block_with_data->end_timestamp;
  }

  return absl::OkStatus();
}

}  // namespace

absl::Status ParameterBlockGenerator::Initialize(
    const absl::flat_hash_map<DecodedUleb128, AudioElementWithData>&
        audio_elements,
    const absl::flat_hash_map<DecodedUleb128, const ParamDefinition*>&
        param_definitions) {
  for (const auto [parameter_id, param_definition] : param_definitions) {
    auto [iter, inserted] = parameter_id_to_metadata_.insert(
        {parameter_id, PerIdParameterMetadata()});
    if (inserted) {
      // Create a new entry.
      auto& per_id_metadata = iter->second;
      RETURN_IF_NOT_OK(GetPerIdMetadata(parameter_id, audio_elements,
                                        param_definition, per_id_metadata));
    }

    const auto param_definition_type = iter->second.param_definition_type;
    if (param_definition_type !=
            ParamDefinition::kParameterDefinitionDemixing &&
        param_definition_type != ParamDefinition::kParameterDefinitionMixGain &&
        param_definition_type !=
            ParamDefinition::kParameterDefinitionReconGain) {
      return absl::InvalidArgumentError(
          absl::StrCat("Unsupported parameter type: ", param_definition_type));
    }
  }

  return absl::OkStatus();
}

absl::Status ParameterBlockGenerator::AddMetadata(
    const iamf_tools_cli_proto::ParameterBlockObuMetadata&
        parameter_block_metadata) {
  const auto& per_id_metadata_iter =
      parameter_id_to_metadata_.find(parameter_block_metadata.parameter_id());
  if (per_id_metadata_iter == parameter_id_to_metadata_.end()) {
    return absl::InvalidArgumentError(
        absl::StrCat("No per-id parameter metadata found for parameter ID= ",
                     parameter_block_metadata.parameter_id()));
  }
  auto& per_id_metadata =
      parameter_id_to_metadata_.at(parameter_block_metadata.parameter_id());

  typed_proto_metadata_[per_id_metadata.param_definition_type].push_back(
      parameter_block_metadata);

  return absl::OkStatus();
}

absl::Status ParameterBlockGenerator::GenerateDemixing(
    GlobalTimingModule& global_timing_module,
    std::list<ParameterBlockWithData>& output_parameter_blocks) {
  RETURN_IF_NOT_OK(GenerateParameterBlocks(
      /*id_to_labeled_frame=*/nullptr,
      /*id_to_labeled_decoded_frame=*/nullptr,
      typed_proto_metadata_[ParamDefinition::kParameterDefinitionDemixing],
      global_timing_module, output_parameter_blocks));

  return absl::OkStatus();
}

absl::Status ParameterBlockGenerator::GenerateMixGain(
    GlobalTimingModule& global_timing_module,
    std::list<ParameterBlockWithData>& output_parameter_blocks) {
  RETURN_IF_NOT_OK(GenerateParameterBlocks(
      /*id_to_labeled_frame=*/nullptr,
      /*id_to_labeled_decoded_frame=*/nullptr,
      typed_proto_metadata_[ParamDefinition::kParameterDefinitionMixGain],
      global_timing_module, output_parameter_blocks));

  return absl::OkStatus();
}

// TODO(b/306319126): Generate Recon Gain iteratively now that the audio frame
//                    decoder decodes iteratively.
absl::Status ParameterBlockGenerator::GenerateReconGain(
    const IdLabeledFrameMap& id_to_labeled_frame,
    const IdLabeledFrameMap& id_to_labeled_decoded_frame,
    GlobalTimingModule& global_timing_module,
    std::list<ParameterBlockWithData>& output_parameter_blocks) {
  RETURN_IF_NOT_OK(GenerateParameterBlocks(
      &id_to_labeled_frame, &id_to_labeled_decoded_frame,
      typed_proto_metadata_[ParamDefinition::kParameterDefinitionReconGain],
      global_timing_module, output_parameter_blocks));
  return absl::OkStatus();
}

absl::Status ParameterBlockGenerator::GenerateParameterBlocks(
    const IdLabeledFrameMap* id_to_labeled_frame,
    const IdLabeledFrameMap* id_to_labeled_decoded_frame,
    std::list<iamf_tools_cli_proto::ParameterBlockObuMetadata>&
        proto_metadata_list,
    GlobalTimingModule& global_timing_module,
    std::list<ParameterBlockWithData>& output_parameter_blocks) {
  for (auto& parameter_block_metadata : proto_metadata_list) {
    ParameterBlockWithData output_parameter_block;
    auto& per_id_metadata =
        parameter_id_to_metadata_.at(parameter_block_metadata.parameter_id());
    RETURN_IF_NOT_OK(PopulateCommonFields(parameter_block_metadata,
                                          per_id_metadata, global_timing_module,
                                          output_parameter_block));

    RETURN_IF_NOT_OK(PopulateSubblocks(
        parameter_block_metadata, override_computed_recon_gains_,
        additional_recon_gains_logging_, id_to_labeled_frame,
        id_to_labeled_decoded_frame, per_id_metadata, output_parameter_block));

    // Disable some verbose logging after the first recon gain block is
    // produced.
    if (!override_computed_recon_gains_) {
      additional_recon_gains_logging_ = false;
    }

    output_parameter_blocks.push_back(std::move(output_parameter_block));
  }

  RETURN_IF_NOT_OK(LogParameterBlockObus(output_parameter_blocks));

  // Clear the metadata of this frame.
  proto_metadata_list.clear();

  return absl::OkStatus();
}

}  // namespace iamf_tools
