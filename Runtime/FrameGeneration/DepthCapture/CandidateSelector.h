#pragma once
#include <cstdint>
#include <unordered_map>
#include <limits>

namespace asteria_depth_capture
{
struct DrawObservation
{
    uint64_t image = 0;
    uint64_t frameID = 0;
    uint64_t drawCalls = 0;
    uint64_t vertices = 0;
    uint32_t width = 0, height = 0, samples = 0;
    bool formatD24S8 = false;
    bool copySourceAvailable = false;
    bool primaryTargetDimensionsMatch = false;
    bool applicationDrawOutsideEffects = false;
};

struct Candidate
{
    uint64_t image = 0;
    uint64_t frameID = 0;
    uint64_t drawCalls = 0;
    uint64_t vertices = 0;
    bool ambiguous = false;
    bool candidateOnly = true;
    bool scenePassConfirmed = false;
};

class CandidateSelector
{
    uint64_t frame = 0;
    uint32_t width = 0, height = 0;
    std::unordered_map<uint64_t, Candidate> candidates;
    static uint64_t Add(uint64_t left, uint64_t right)
    {
        return std::numeric_limits<uint64_t>::max() - left < right ? std::numeric_limits<uint64_t>::max() : left + right;
    }
public:
    void BeginFrame(uint64_t id, uint32_t mainWidth, uint32_t mainHeight)
    {
        frame = id; width = mainWidth; height = mainHeight;
        candidates.clear();
    }
    bool Observe(const DrawObservation &observation)
    {
        if (!observation.image || observation.frameID != frame || !width || !height ||
            observation.width != width || observation.height != height || observation.samples != 4 ||
            !observation.formatD24S8 || !observation.copySourceAvailable || !observation.primaryTargetDimensionsMatch ||
            !observation.applicationDrawOutsideEffects || !observation.drawCalls) return false;
        auto &candidate = candidates[observation.image];
        candidate.image = observation.image;
        candidate.frameID = frame;
        candidate.drawCalls = Add(candidate.drawCalls, observation.drawCalls);
        candidate.vertices = Add(candidate.vertices, observation.vertices);
        return true;
    }
    Candidate Select() const
    {
        Candidate best{};
        for (const auto &entry : candidates)
        {
            const auto &candidate = entry.second;
            if (!best.image || candidate.drawCalls > best.drawCalls ||
                (candidate.drawCalls == best.drawCalls && candidate.vertices > best.vertices)) best = candidate;
        }
        if (!best.image) return best;
        for (const auto &entry : candidates)
            if (entry.first != best.image && entry.second.drawCalls >= best.drawCalls - best.drawCalls / 20)
                best.ambiguous = true;
        return best;
    }
};
}
