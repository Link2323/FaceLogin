#include "enrollment_templates.h"

#include "../common/logger.h"
#include "../storage/credential_store.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace facelogin {
namespace {
bool BuildAngleTemplate(const EnrollmentSamples& samples,
                        const EnrollmentSamples::AngleGroup& g,
                        std::vector<float>& out) {
    const auto& embeddings = samples.embeddings;
    const size_t dim = embeddings.front().size();
    // Embedding consistency check: verify all samples in this angle group
    // are from the same person. Compute average pairwise distance — if it
    // exceeds the cap, reject.
    // SFace trial uses the bounded 1.00 consistency cap for each angle.
    // It is independent of cross-angle registration and does not average poses.
    {
        double totalDist = 0.0;
        int pairs = 0;
        for (size_t i = g.begin; i < g.begin + g.count; i++) {
            for (size_t j = i + 1; j < g.begin + g.count; j++) {
                double sum = 0.0;
                for (size_t k = 0; k < dim; k++) {
                    double diff = embeddings[i][k] - embeddings[j][k];
                    sum += diff * diff;
                }
                totalDist += std::sqrt(sum);
                pairs++;
            }
        }
        double avgPairDist = (pairs > 0) ? totalDist / pairs : 0.0;
        float maxAllowed = EmbeddingThresholdForDim(kDefaultMatchThreshold, dim);
        FACELOGIN_INFO(L"Enrollment consistency [%ls]: avg pairwise dist=%.4f (max=%.3f, %d pairs, %zu-D)",
                      g.label, avgPairDist, maxAllowed, pairs, dim);
        if (avgPairDist > maxAllowed) {
            FACELOGIN_ERROR(L"Enrollment consistency check failed [%ls]: avg pairwise dist %.4f > %.3f. "
                           L"Samples may be from different faces.", g.label, avgPairDist, maxAllowed);
            return false;
        }
    }

    // Average this group's samples. Initialize to the samples'
    // dimensionality — the vectors require matching sizes.
    std::vector<float> avgEmbedding(dim, 0.0f);
    for (size_t i = g.begin; i < g.begin + g.count; i++) {
        const auto& emb = embeddings[i];
        for (size_t k = 0; k < dim; k++) avgEmbedding[k] += emb[k];
    }
    for (float& v : avgEmbedding) v /= static_cast<float>(g.count);
    double squaredNorm = 0.0;
    for (float v : avgEmbedding) squaredNorm += static_cast<double>(v) * v;
    const double averageNorm = std::sqrt(squaredNorm);
    if (!std::isfinite(averageNorm) || averageNorm <= 1e-8) {
        FACELOGIN_ERROR(L"Enrollment template preparation: invalid mean norm [%ls]", g.label);
        return false;
    }
    for (float& v : avgEmbedding) v = static_cast<float>(v / averageNorm);

    out = std::move(avgEmbedding);
    return true;
}
} // namespace

bool BuildEnrollmentTemplates(const EnrollmentSamples& samples,
                              std::vector<EnrollmentTemplate>& out) {
    out.clear();
    if (samples.embeddings.empty() || samples.groups.empty()) {
        FACELOGIN_ERROR(L"Enrollment template preparation: no samples");
        return false;
    }
    const size_t dim = samples.embeddings.front().size();
    // Check before pairwise indexing. Reject malformed input as a whole;
    // never silently skip a short embedding or average non-finite values.
    for (const auto& embedding : samples.embeddings) {
        if (dim == 0 || embedding.size() != dim ||
            !std::all_of(embedding.begin(), embedding.end(),
                         [](float value) { return std::isfinite(value); })) {
            FACELOGIN_ERROR(L"Enrollment template preparation: invalid embedding");
            return false;
        }
    }
    std::vector<EnrollmentTemplate> prepared;
    size_t next = 0;
    for (const auto& group : samples.groups) {
        if (group.begin != next || group.count == 0 ||
            group.count > samples.embeddings.size() - next || !group.label) {
            FACELOGIN_ERROR(L"Enrollment template preparation: invalid angle group");
            return false;
        }
        EnrollmentTemplate item;
        item.label = group.label;
        item.targetYaw = group.targetYaw;
        if (!BuildAngleTemplate(samples, group, item.embedding)) return false;
        prepared.push_back(std::move(item));
        next += group.count;
    }
    if (next != samples.embeddings.size()) {
        FACELOGIN_ERROR(L"Enrollment template preparation: ungrouped samples");
        return false;
    }
    out = std::move(prepared);
    return true;
}

} // namespace facelogin
