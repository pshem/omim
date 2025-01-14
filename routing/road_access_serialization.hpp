#pragma once

#include "routing/coding.hpp"
#include "routing/road_access.hpp"
#include "routing/road_point.hpp"
#include "routing/segment.hpp"
#include "routing/vehicle_mask.hpp"

#include "routing_common/num_mwm_id.hpp"

#include "coding/bit_streams.hpp"
#include "coding/reader.hpp"
#include "coding/varint.hpp"
#include "coding/write_to_sink.hpp"

#include "base/assert.hpp"
#include "base/checked_cast.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include "3party/skarupke/flat_hash_map.hpp"

namespace routing
{
class RoadAccessSerializer final
{
public:
  using WayToAccess = RoadAccess::WayToAccess;
  using PointToAccess = RoadAccess::PointToAccess;
  using RoadAccessByVehicleType = std::array<RoadAccess, static_cast<size_t>(VehicleType::Count)>;

  RoadAccessSerializer() = delete;

  template <class Sink>
  static void Serialize(Sink & sink, RoadAccessByVehicleType const & roadAccessByType)
  {
    uint32_t const header = kLatestVersion;
    WriteToSink(sink, header);
    SerializeAccess(sink, roadAccessByType);
  }

  template <class Source>
  static void Deserialize(Source & src, VehicleType vehicleType, RoadAccess & roadAccess)
  {
    uint32_t const header = ReadPrimitiveFromSource<uint32_t>(src);
    CHECK_EQUAL(header, kLatestVersion, ());
    DeserializeAccess(src, vehicleType, roadAccess);
  }

private:
  inline static uint32_t const kLatestVersion = 1;

  template <class Sink>
  static void SerializeAccess(Sink & sink, RoadAccessByVehicleType const & roadAccessByType)
  {
    auto const sectionSizesPos = sink.Pos();
    std::array<uint32_t, static_cast<size_t>(VehicleType::Count)> sectionSizes;
    for (size_t i = 0; i < sectionSizes.size(); ++i)
    {
      sectionSizes[i] = 0;
      WriteToSink(sink, sectionSizes[i]);
    }

    for (size_t i = 0; i < static_cast<size_t>(VehicleType::Count); ++i)
    {
      auto const pos = sink.Pos();
      SerializeOneVehicleType(sink, roadAccessByType[i].GetWayToAccess(),
                              roadAccessByType[i].GetPointToAccess());
      sectionSizes[i] = base::checked_cast<uint32_t>(sink.Pos() - pos);
    }

    auto const endPos = sink.Pos();
    sink.Seek(sectionSizesPos);
    for (auto const sectionSize : sectionSizes)
      WriteToSink(sink, sectionSize);

    sink.Seek(endPos);
  }

  template <class Source>
  static void DeserializeAccess(Source & src, VehicleType vehicleType, RoadAccess & roadAccess)
  {
    std::array<uint32_t, static_cast<size_t>(VehicleType::Count)> sectionSizes{};
    static_assert(static_cast<int>(VehicleType::Count) == 4,
                  "It is assumed below that there are only 4 vehicle types and we store 4 numbers "
                  "of sections size. If you add or remove vehicle type you should up "
                  "|kLatestVersion| and save back compatibility here.");

    for (auto & sectionSize : sectionSizes)
      sectionSize = ReadPrimitiveFromSource<uint32_t>(src);

    for (size_t i = 0; i < sectionSizes.size(); ++i)
    {
      auto const sectionVehicleType = static_cast<VehicleType>(i);
      if (sectionVehicleType != vehicleType)
      {
        src.Skip(sectionSizes[i]);
        continue;
      }

      WayToAccess wayToAccess;
      PointToAccess pointToAccess;
      DeserializeOneVehicleType(src, wayToAccess, pointToAccess);

      roadAccess.SetAccess(std::move(wayToAccess), std::move(pointToAccess));
      return;
    }
  }

  template <typename Sink>
  static void SerializeOneVehicleType(Sink & sink, WayToAccess const & wayToAccess,
                                      PointToAccess const & pointToAccess)
  {
    std::array<std::vector<Segment>, static_cast<size_t>(RoadAccess::Type::Count)>
        segmentsByRoadAccessType;
    for (auto const & kv : wayToAccess)
    {
      segmentsByRoadAccessType[static_cast<size_t>(kv.second)].push_back(
          Segment(kFakeNumMwmId, kv.first, 0 /* wildcard segmentIdx */, true /* widcard forward */));
    }
    // For nodes we store |pointId + 1| because 0 is reserved for wildcard segmentIdx.
    for (auto const & kv : pointToAccess)
    {
      segmentsByRoadAccessType[static_cast<size_t>(kv.second)].push_back(
          Segment(kFakeNumMwmId, kv.first.GetFeatureId(), kv.first.GetPointId() + 1, true));
    }

    for (auto & segs : segmentsByRoadAccessType)
    {
      std::sort(segs.begin(), segs.end());
      SerializeSegments(sink, segs);
    }
  }

  template <typename Source>
  static void DeserializeOneVehicleType(Source & src, WayToAccess & wayToAccess,
                                        PointToAccess & pointToAccess)
  {
    wayToAccess.clear();
    pointToAccess.clear();
    for (size_t i = 0; i < static_cast<size_t>(RoadAccess::Type::Count); ++i)
    {
      // An earlier version allowed blocking any segment of a feature (or the entire feature
      // by providing a wildcard segment index).
      // At present, we either block the feature entirely or block any of its road points. The
      // the serialization code remains the same, although its semantics changes as we now
      // work with point indices instead of segment indices.
      std::vector<Segment> segs;
      DeserializeSegments(src, segs);
      for (auto const & seg : segs)
      {
        if (seg.GetSegmentIdx() == 0)
        {
          // Wildcard segmentIdx.
          wayToAccess[seg.GetFeatureId()] = static_cast<RoadAccess::Type>(i);
        }
        else
        {
          // For nodes we store |pointId + 1| because 0 is reserved for wildcard segmentIdx.
          pointToAccess[RoadPoint(seg.GetFeatureId(), seg.GetSegmentIdx() - 1)] =
              static_cast<RoadAccess::Type>(i);
        }
      }
    }
  }

  // todo(@m) This code borrows heavily from traffic/traffic_info.hpp:SerializeTrafficKeys.
  template <typename Sink>
  static void SerializeSegments(Sink & sink, std::vector<Segment> const & segments)
  {
    std::vector<uint32_t> featureIds(segments.size());
    std::vector<uint32_t> segmentIndices(segments.size());
    std::vector<bool> isForward(segments.size());

    for (size_t i = 0; i < segments.size(); ++i)
    {
      auto const & seg = segments[i];
      CHECK_EQUAL(seg.GetMwmId(), kFakeNumMwmId,
                  ("Numeric mwm ids are temporary and must not be serialized."));
      featureIds[i] = seg.GetFeatureId();
      segmentIndices[i] = seg.GetSegmentIdx();
      isForward[i] = seg.IsForward();
    }

    WriteVarUint(sink, segments.size());

    {
      BitWriter<Sink> bitWriter(sink);

      uint32_t prevFid = 0;
      for (auto const fid : featureIds)
      {
        CHECK_GREATER_OR_EQUAL(fid, prevFid, ());
        uint64_t const fidDiff = static_cast<uint64_t>(fid - prevFid);
        WriteGamma(bitWriter, fidDiff + 1);
        prevFid = fid;
      }

      for (auto const idx : segmentIndices)
        WriteGamma(bitWriter, idx + 1);

      for (auto const val : isForward)
        bitWriter.Write(val ? 1 : 0, 1 /* numBits */);
    }
  }

  template <typename Source>
  static void DeserializeSegments(Source & src, std::vector<Segment> & segments)
  {
    auto const n = static_cast<size_t>(ReadVarUint<uint64_t>(src));

    std::vector<uint32_t> featureIds(n);
    std::vector<uint32_t> segmentIndices(n);
    std::vector<bool> isForward(n);

    BitReader<Source> bitReader(src);
    uint32_t prevFid = 0;
    for (size_t i = 0; i < n; ++i)
    {
      prevFid += ReadGamma<uint64_t>(bitReader) - 1;
      featureIds[i] = prevFid;
    }

    for (size_t i = 0; i < n; ++i)
      segmentIndices[i] = ReadGamma<uint32_t>(bitReader) - 1;
    for (size_t i = 0; i < n; ++i)
      isForward[i] = bitReader.Read(1) > 0;

    // Read the padding bits.
    auto bitsRead = bitReader.BitsRead();
    while (bitsRead % CHAR_BIT != 0)
    {
      bitReader.Read(1);
      ++bitsRead;
    }

    segments.clear();
    segments.reserve(n);
    for (size_t i = 0; i < n; ++i)
      segments.emplace_back(kFakeNumMwmId, featureIds[i], segmentIndices[i], isForward[i]);
  }
};
}  // namespace routing
