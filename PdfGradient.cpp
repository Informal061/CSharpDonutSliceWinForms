#include "pch.h"
#include "PdfGradient.h"
#include "PdfDocument.h"
#include "PdfDebug.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <set>

namespace pdf
{
    // =====================================================
    // Forward declarations
    // =====================================================
    static bool parseFunctionType0(
        const std::shared_ptr<PdfDictionary>& funcDict,
        const std::shared_ptr<PdfStream>& funcStream,
        PdfDocument* doc,
        std::vector<GradientStop>& outStops,
        int numComponents);

    static bool parseFunctionType2(
        const std::shared_ptr<PdfDictionary>& funcDict,
        PdfDocument* doc,
        std::vector<GradientStop>& outStops,
        int numComponents);

    static bool parseFunctionType3(
        const std::shared_ptr<PdfDictionary>& funcDict,
        PdfDocument* doc,
        std::vector<GradientStop>& outStops,
        int numComponents);

    // =====================================================
    // Yardımcı fonksiyonlar
    // =====================================================

    static void cmykToRgb(double c, double m, double y, double k, double outRgb[3])
    {
        outRgb[0] = (1.0 - c) * (1.0 - k);
        outRgb[1] = (1.0 - m) * (1.0 - k);
        outRgb[2] = (1.0 - y) * (1.0 - k);

        outRgb[0] = std::clamp(outRgb[0], 0.0, 1.0);
        outRgb[1] = std::clamp(outRgb[1], 0.0, 1.0);
        outRgb[2] = std::clamp(outRgb[2], 0.0, 1.0);
    }

    static void colorToRGB(const double* color, int numComponents, double outRgb[3])
    {
        if (numComponents == 1)
        {
            double gray = color[0];
            outRgb[0] = gray;
            outRgb[1] = gray;
            outRgb[2] = gray;
        }
        else if (numComponents == 3)
        {
            outRgb[0] = color[0];
            outRgb[1] = color[1];
            outRgb[2] = color[2];
        }
        else if (numComponents == 4)
        {
            cmykToRgb(color[0], color[1], color[2], color[3], outRgb);
        }
        else
        {
            double avg = 0;
            for (int i = 0; i < numComponents; ++i)
                avg += color[i];
            avg /= numComponents;
            outRgb[0] = outRgb[1] = outRgb[2] = avg;
        }

        outRgb[0] = std::clamp(outRgb[0], 0.0, 1.0);
        outRgb[1] = std::clamp(outRgb[1], 0.0, 1.0);
        outRgb[2] = std::clamp(outRgb[2], 0.0, 1.0);
    }

    // =====================================================
    // PdfGradient implementation
    // =====================================================

    void PdfGradient::evaluateColor(double t, double outRgb[3]) const
    {
        t = std::clamp(t, 0.0, 1.0);

        if (stops.empty())
        {
            outRgb[0] = outRgb[1] = outRgb[2] = 0.0;
            return;
        }

        if (stops.size() == 1)
        {
            outRgb[0] = stops[0].rgb[0];
            outRgb[1] = stops[0].rgb[1];
            outRgb[2] = stops[0].rgb[2];
            return;
        }

        // t'nin hangi stop aralığında olduğunu bul
        size_t leftIdx = 0;
        size_t rightIdx = 1;

        for (size_t i = 0; i + 1 < stops.size(); ++i)
        {
            if (t >= stops[i].position && t <= stops[i + 1].position)
            {
                leftIdx = i;
                rightIdx = i + 1;
                break;
            }
        }

        // Son aralıkta olabilir
        if (t > stops[stops.size() - 2].position)
        {
            leftIdx = stops.size() - 2;
            rightIdx = stops.size() - 1;
        }

        double t0 = stops[leftIdx].position;
        double t1 = stops[rightIdx].position;
        double range = t1 - t0;

        if (range < 1e-10)
        {
            outRgb[0] = stops[leftIdx].rgb[0];
            outRgb[1] = stops[leftIdx].rgb[1];
            outRgb[2] = stops[leftIdx].rgb[2];
            return;
        }

        double f = (t - t0) / range;
        f = std::clamp(f, 0.0, 1.0);

        const double* leftRgb = stops[leftIdx].rgb;
        const double* rightRgb = stops[rightIdx].rgb;

        outRgb[0] = leftRgb[0] + f * (rightRgb[0] - leftRgb[0]);
        outRgb[1] = leftRgb[1] + f * (rightRgb[1] - leftRgb[1]);
        outRgb[2] = leftRgb[2] + f * (rightRgb[2] - leftRgb[2]);
    }

    bool PdfGradient::parseFunction(
        const std::shared_ptr<PdfObject>& funcObj,
        PdfDocument* doc,
        std::vector<GradientStop>& outStops)
    {
        return parseFunctionWithColorSpace(funcObj, doc, outStops, 3);
    }

    bool PdfGradient::parseFunctionWithColorSpace(
        const std::shared_ptr<PdfObject>& funcObj,
        PdfDocument* doc,
        std::vector<GradientStop>& outStops,
        int numComponents)
    {
        if (!funcObj || !doc) return false;

        std::set<int> visited;
        auto resolved = doc->resolve(funcObj, visited);

        if (!resolved)
        {
            LogDebug("ERROR: Could not resolve function object");
            return false;
        }

        // Function bir dictionary veya stream olabilir
        std::shared_ptr<PdfDictionary> funcDict;
        std::shared_ptr<PdfStream> funcStream;

        if (auto stream = std::dynamic_pointer_cast<PdfStream>(resolved))
        {
            funcStream = stream;
            funcDict = stream->dict;
        }
        else if (auto dict = std::dynamic_pointer_cast<PdfDictionary>(resolved))
        {
            funcDict = dict;
        }
        else
        {
            LogDebug("WARNING: Function is not a dictionary or stream");
            return false;
        }

        if (!funcDict)
        {
            LogDebug("WARNING: No function dictionary");
            return false;
        }

        // FunctionType
        visited.clear();
        auto typeObj = std::dynamic_pointer_cast<PdfNumber>(
            doc->resolve(funcDict->get("/FunctionType"), visited));

        if (!typeObj)
        {
            LogDebug("WARNING: No FunctionType");
            return false;
        }

        int funcType = (int)typeObj->value;

        LogDebug("Function type: %d, numComponents: %d", funcType, numComponents);

        if (funcType == 0)
        {
            return parseFunctionType0(funcDict, funcStream, doc, outStops, numComponents);
        }
        else if (funcType == 2)
        {
            return parseFunctionType2(funcDict, doc, outStops, numComponents);
        }
        else if (funcType == 3)
        {
            return parseFunctionType3(funcDict, doc, outStops, numComponents);
        }
        else if (funcType == 4)
        {
            LogDebug("WARNING: PostScript calculator function (Type 4) not supported");
            return false;
        }

        LogDebug("WARNING: Unknown function type: %d", funcType);
        return false;
    }

    static double cubicInterpolate(double p0, double p1, double p2, double p3, double t)
    {
        double t2 = t * t;
        double t3 = t2 * t;

        return 0.5 * ((2.0 * p1) +
                      (-p0 + p2) * t +
                      (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t2 +
                      (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * t3);
    }

    // =====================================================
    // Function Type 0 - Sampled Function
    // =====================================================
    static bool parseFunctionType0(
        const std::shared_ptr<PdfDictionary>& funcDict,
        const std::shared_ptr<PdfStream>& funcStream,
        PdfDocument* doc,
        std::vector<GradientStop>& outStops,
        int numComponents)
    {
        std::set<int> visited;

        LogDebug("--- parseFunctionType0 START ---");

        // Stream data gerekli
        if (!funcStream)
        {
            LogDebug("WARNING: Type 0 function requires stream data");
            return false;
        }

        // Size array (required) - sample sayısı
        visited.clear();
        auto sizeArr = std::dynamic_pointer_cast<PdfArray>(
            doc->resolve(funcDict->get("/Size"), visited));

        if (!sizeArr || sizeArr->items.empty())
        {
            LogDebug("WARNING: No /Size array");
            return false;
        }

        visited.clear();
        auto sizeNum = std::dynamic_pointer_cast<PdfNumber>(
            doc->resolve(sizeArr->items[0], visited));

        int numSamples = sizeNum ? (int)sizeNum->value : 2;
        LogDebug("Number of samples: %d", numSamples);

        // BitsPerSample (required)
        visited.clear();
        auto bpsObj = std::dynamic_pointer_cast<PdfNumber>(
            doc->resolve(funcDict->get("/BitsPerSample"), visited));

        int bitsPerSample = bpsObj ? (int)bpsObj->value : 8;
        LogDebug("BitsPerSample: %d", bitsPerSample);

        // Order (optional)
        visited.clear();
        auto orderObj = std::dynamic_pointer_cast<PdfNumber>(
            doc->resolve(funcDict->get("/Order"), visited));
        int interpolationOrder = orderObj ? (int)orderObj->value : 1;
        LogDebug("Interpolation Order: %d", interpolationOrder);

        // Range (required for gradient) - output value range
        visited.clear();
        auto rangeArr = std::dynamic_pointer_cast<PdfArray>(
            doc->resolve(funcDict->get("/Range"), visited));

        std::vector<double> rangeMin, rangeMax;
        int outputComponents = numComponents;

        if (rangeArr && rangeArr->items.size() >= 2)
        {
            outputComponents = (int)rangeArr->items.size() / 2;
            for (int i = 0; i < outputComponents; ++i)
            {
                visited.clear();
                auto rmin = std::dynamic_pointer_cast<PdfNumber>(
                    doc->resolve(rangeArr->items[i * 2], visited));
                visited.clear();
                auto rmax = std::dynamic_pointer_cast<PdfNumber>(
                    doc->resolve(rangeArr->items[i * 2 + 1], visited));

                rangeMin.push_back(rmin ? rmin->value : 0.0);
                rangeMax.push_back(rmax ? rmax->value : 1.0);
            }
        }
        else
        {
            for (int i = 0; i < outputComponents; ++i)
            {
                rangeMin.push_back(0.0);
                rangeMax.push_back(1.0);
            }
        }

        LogDebug("Output components: %d", outputComponents);

        // Decode array (optional) - sample to output mapping
        visited.clear();
        auto decodeArr = std::dynamic_pointer_cast<PdfArray>(
            doc->resolve(funcDict->get("/Decode"), visited));

        std::vector<double> decodeMin, decodeMax;
        if (decodeArr && decodeArr->items.size() >= (size_t)(outputComponents * 2))
        {
            for (int i = 0; i < outputComponents; ++i)
            {
                visited.clear();
                auto dmin = std::dynamic_pointer_cast<PdfNumber>(
                    doc->resolve(decodeArr->items[i * 2], visited));
                visited.clear();
                auto dmax = std::dynamic_pointer_cast<PdfNumber>(
                    doc->resolve(decodeArr->items[i * 2 + 1], visited));

                decodeMin.push_back(dmin ? dmin->value : rangeMin[i]);
                decodeMax.push_back(dmax ? dmax->value : rangeMax[i]);
            }
        }
        else
        {
            decodeMin = rangeMin;
            decodeMax = rangeMax;
        }

        // ✅ Stream'i decode et - PdfDocument::decodeStream kullan
        std::vector<uint8_t> data;
        bool decodeSuccess = doc->decodeStream(funcStream, data);

        LogDebug("decodeStream result: %s, decoded size: %zu, raw size: %zu",
            decodeSuccess ? "SUCCESS" : "FAILED",
            data.size(),
            funcStream->data.size());

        if (!decodeSuccess || data.empty())
        {
            // Decode başarısız olursa raw data kullan
            data = funcStream->data;
            LogDebug("Using RAW stream data instead");
        }

        LogDebug("Stream data size: %zu bytes", data.size());

        // İlk 30 byte'ı hex olarak logla
        std::string hexDump;
        for (size_t i = 0; i < std::min(data.size(), (size_t)30); ++i)
        {
            char buf[8];
            snprintf(buf, sizeof(buf), "%02X ", data[i]);
            hexDump += buf;
        }
        LogDebug("First bytes: %s", hexDump.c_str());

        if (data.empty())
        {
            LogDebug("WARNING: Empty stream data");
            return false;
        }

        // Sample değerlerini oku
        double maxSampleValue = (1 << bitsPerSample) - 1;
        int bytesPerSample = (bitsPerSample + 7) / 8;
        int bytesPerEntry = bytesPerSample * outputComponents;

        LogDebug("maxSampleValue: %.0f, bytesPerEntry: %d", maxSampleValue, bytesPerEntry);

        // Tüm ham sample değerlerini oku
        std::vector<std::vector<double>> rawSamples;
        rawSamples.reserve(numSamples);

        for (int sampleIdx = 0; sampleIdx < numSamples; ++sampleIdx)
        {
            int offset = sampleIdx * bytesPerEntry;
            if (offset + bytesPerEntry > (int)data.size()) break;

            std::vector<double> outputValues(outputComponents);
            for (int c = 0; c < outputComponents; ++c)
            {
                int byteOffset = offset + c * bytesPerSample;
                uint32_t rawValue = 0;
                for (int b = 0; b < bytesPerSample; ++b)
                {
                    rawValue = (rawValue << 8) | data[byteOffset + b];
                }
                double normalized = rawValue / maxSampleValue;
                outputValues[c] = decodeMin[c] + normalized * (decodeMax[c] - decodeMin[c]);
            }
            rawSamples.push_back(outputValues);
        }

        if (rawSamples.empty()) return false;

        // Order 3 (Cubic) ise upsampling yap
        if (interpolationOrder == 3 && numSamples >= 4)
        {
            // Her segmenti N parçaya böl
            const int subSteps = 4;

            for (int i = 0; i < numSamples - 1; ++i)
            {
                // Control points for Catmull-Rom
                // p0, p1 (current), p2 (next), p3
                int idx0 = std::max(0, i - 1);
                int idx1 = i;
                int idx2 = std::min(numSamples - 1, i + 1);
                int idx3 = std::min(numSamples - 1, i + 2);

                const auto& p0 = rawSamples[idx0];
                const auto& p1 = rawSamples[idx1];
                const auto& p2 = rawSamples[idx2];
                const auto& p3 = rawSamples[idx3];

                for (int s = 0; s < subSteps; ++s)
                {
                    double t = (double)s / subSteps;

                    // Global pozisyon
                    double globalT = (double)(i * subSteps + s) / ((numSamples - 1) * subSteps);

                    std::vector<double> interpolated(outputComponents);
                    for(int c=0; c<outputComponents; ++c)
                    {
                        interpolated[c] = cubicInterpolate(p0[c], p1[c], p2[c], p3[c], t);
                        // Clamp
                        interpolated[c] = std::clamp(interpolated[c], decodeMin[c], decodeMax[c]);
                    }

                    GradientStop stop;
                    stop.position = globalT;
                    colorToRGB(interpolated.data(), outputComponents, stop.rgb);
                    outStops.push_back(stop);
                }
            }
            // Son noktayı ekle
            GradientStop lastStop;
            lastStop.position = 1.0;
            colorToRGB(rawSamples.back().data(), outputComponents, lastStop.rgb);
            outStops.push_back(lastStop);
        }
        else
        {
            // Linear (Order 1) veya çok az sample varsa direkt ekle
            for (int i = 0; i < (int)rawSamples.size(); ++i)
            {
                GradientStop stop;
                stop.position = (numSamples > 1) ? (double)i / (numSamples - 1) : 0.0;
                colorToRGB(rawSamples[i].data(), outputComponents, stop.rgb);
                outStops.push_back(stop);
            }
        }

        if (outStops.empty()) return false;

        // Pozisyona göre sırala
        std::sort(outStops.begin(), outStops.end(),
            [](const GradientStop& a, const GradientStop& b) {
                return a.position < b.position;
            });

        LogDebug("--- parseFunctionType0 END: %zu stops ---", outStops.size());
        return !outStops.empty();
    }

    // =====================================================
    // Function Type 2 - Exponential Interpolation
    // =====================================================
    static bool parseFunctionType2(
        const std::shared_ptr<PdfDictionary>& funcDict,
        PdfDocument* doc,
        std::vector<GradientStop>& outStops,
        int numComponents)
    {
        std::set<int> visited;

        LogDebug("--- parseFunctionType2 START ---");

        // N (exponent) - default 1
        double N = 1.0;
        auto nObj = std::dynamic_pointer_cast<PdfNumber>(
            doc->resolve(funcDict->get("/N"), visited));
        if (nObj) N = nObj->value;

        LogDebug("Exponent N = %.2f", N);

        // C0 (start color)
        std::vector<double> c0(numComponents, 0.0);

        visited.clear();
        auto c0Arr = std::dynamic_pointer_cast<PdfArray>(
            doc->resolve(funcDict->get("/C0"), visited));

        if (c0Arr)
        {
            c0.resize(c0Arr->items.size());
            for (size_t i = 0; i < c0Arr->items.size(); ++i)
            {
                visited.clear();
                if (auto n = std::dynamic_pointer_cast<PdfNumber>(
                    doc->resolve(c0Arr->items[i], visited)))
                {
                    c0[i] = n->value;
                }
            }
        }

        // C1 (end color)
        std::vector<double> c1(numComponents, 1.0);

        visited.clear();
        auto c1Arr = std::dynamic_pointer_cast<PdfArray>(
            doc->resolve(funcDict->get("/C1"), visited));

        if (c1Arr)
        {
            c1.resize(c1Arr->items.size());
            for (size_t i = 0; i < c1Arr->items.size(); ++i)
            {
                visited.clear();
                if (auto n = std::dynamic_pointer_cast<PdfNumber>(
                    doc->resolve(c1Arr->items[i], visited)))
                {
                    c1[i] = n->value;
                }
            }
        }

        // Log C0 ve C1
        LogDebug("C0: [%.3f, %.3f, %.3f, ...]",
            c0.size() > 0 ? c0[0] : 0,
            c0.size() > 1 ? c0[1] : 0,
            c0.size() > 2 ? c0[2] : 0);
        LogDebug("C1: [%.3f, %.3f, %.3f, ...]",
            c1.size() > 0 ? c1[0] : 0,
            c1.size() > 1 ? c1[1] : 0,
            c1.size() > 2 ? c1[2] : 0);

        // RGB'ye çevir
        double rgb0[3], rgb1[3];
        colorToRGB(c0.data(), (int)c0.size(), rgb0);
        colorToRGB(c1.data(), (int)c1.size(), rgb1);

        LogDebug("Converted: C0->RGB[%.3f,%.3f,%.3f], C1->RGB[%.3f,%.3f,%.3f]",
            rgb0[0], rgb0[1], rgb0[2], rgb1[0], rgb1[1], rgb1[2]);

        // Non-linear ise ara değerler ekle
        if (std::abs(N - 1.0) > 0.01)
        {
            const int numSteps = 16;
            for (int i = 0; i <= numSteps; ++i)
            {
                double t = (double)i / numSteps;
                double factor = std::pow(t, N);

                GradientStop s;
                s.position = t;
                s.rgb[0] = rgb0[0] + factor * (rgb1[0] - rgb0[0]);
                s.rgb[1] = rgb0[1] + factor * (rgb1[1] - rgb0[1]);
                s.rgb[2] = rgb0[2] + factor * (rgb1[2] - rgb0[2]);
                outStops.push_back(s);
            }
        }
        else
        {
            GradientStop s0, s1;
            s0.position = 0.0;
            s0.rgb[0] = rgb0[0];
            s0.rgb[1] = rgb0[1];
            s0.rgb[2] = rgb0[2];

            s1.position = 1.0;
            s1.rgb[0] = rgb1[0];
            s1.rgb[1] = rgb1[1];
            s1.rgb[2] = rgb1[2];

            outStops.push_back(s0);
            outStops.push_back(s1);
        }

        LogDebug("--- parseFunctionType2 END: %zu stops ---", outStops.size());
        return true;
    }

    // =====================================================
    // Function Type 3 - Stitching Function
    // =====================================================
    static bool parseFunctionType3(
        const std::shared_ptr<PdfDictionary>& funcDict,
        PdfDocument* doc,
        std::vector<GradientStop>& outStops,
        int numComponents)
    {
        std::set<int> visited;

        LogDebug("--- parseFunctionType3 START ---");

        // Functions array (required)
        auto funcsArr = std::dynamic_pointer_cast<PdfArray>(
            doc->resolve(funcDict->get("/Functions"), visited));

        if (!funcsArr || funcsArr->items.empty())
        {
            LogDebug("WARNING: No /Functions array");
            return false;
        }

        LogDebug("Functions array size: %zu", funcsArr->items.size());

        // Bounds array
        visited.clear();
        auto boundsArr = std::dynamic_pointer_cast<PdfArray>(
            doc->resolve(funcDict->get("/Bounds"), visited));

        std::vector<double> bounds;
        if (boundsArr)
        {
            for (auto& item : boundsArr->items)
            {
                visited.clear();
                if (auto n = std::dynamic_pointer_cast<PdfNumber>(
                    doc->resolve(item, visited)))
                {
                    bounds.push_back(n->value);
                }
            }
        }

        // Domain
        visited.clear();
        auto domainArr = std::dynamic_pointer_cast<PdfArray>(
            doc->resolve(funcDict->get("/Domain"), visited));

        double domainMin = 0.0, domainMax = 1.0;
        if (domainArr && domainArr->items.size() >= 2)
        {
            visited.clear();
            if (auto d0 = std::dynamic_pointer_cast<PdfNumber>(
                doc->resolve(domainArr->items[0], visited)))
                domainMin = d0->value;

            visited.clear();
            if (auto d1 = std::dynamic_pointer_cast<PdfNumber>(
                doc->resolve(domainArr->items[1], visited)))
                domainMax = d1->value;
        }

        double domainRange = domainMax - domainMin;
        if (domainRange < 1e-10) domainRange = 1.0;

        for (size_t i = 0; i < funcsArr->items.size(); ++i)
        {
            double subDomainMin = (i == 0) ? domainMin : bounds[i - 1];
            double subDomainMax = (i < bounds.size()) ? bounds[i] : domainMax;

            double posStart = (subDomainMin - domainMin) / domainRange;
            double posEnd = (subDomainMax - domainMin) / domainRange;
            double posRange = posEnd - posStart;

            LogDebug("Sub-function %zu: domain [%.3f, %.3f] -> position [%.3f, %.3f]",
                i, subDomainMin, subDomainMax, posStart, posEnd);

            // Sub-function'ı resolve et
            visited.clear();
            auto subFuncObj = doc->resolve(funcsArr->items[i], visited);

            if (!subFuncObj)
            {
                LogDebug("WARNING: Could not resolve sub-function %zu", i);
                continue;
            }

            // Debug: Sub-function tipini logla
            if (std::dynamic_pointer_cast<PdfDictionary>(subFuncObj))
            {
                LogDebug("  Sub-function %zu is Dictionary", i);
            }
            else if (std::dynamic_pointer_cast<PdfStream>(subFuncObj))
            {
                LogDebug("  Sub-function %zu is Stream", i);
            }
            else if (auto ref = std::dynamic_pointer_cast<PdfIndirectRef>(subFuncObj))
            {
                LogDebug("  WARNING: Sub-function %zu is still Reference (obj %d)", i, ref->objNum);
            }

            std::vector<GradientStop> subStops;

            if (PdfGradient::parseFunctionWithColorSpace(subFuncObj, doc, subStops, numComponents))
            {
                LogDebug("  Sub-function %zu: parsed %zu stops", i, subStops.size());

                for (auto& stop : subStops)
                {
                    GradientStop s;
                    s.position = posStart + stop.position * posRange;
                    s.rgb[0] = stop.rgb[0];
                    s.rgb[1] = stop.rgb[1];
                    s.rgb[2] = stop.rgb[2];

                    // Duplicate kontrolü
                    bool isDuplicate = false;
                    for (const auto& existing : outStops)
                    {
                        if (std::abs(existing.position - s.position) < 1e-6)
                        {
                            isDuplicate = true;
                            break;
                        }
                    }

                    if (!isDuplicate)
                    {
                        outStops.push_back(s);
                        LogDebug("    Added stop: pos=%.3f, rgb=[%.3f,%.3f,%.3f]",
                            s.position, s.rgb[0], s.rgb[1], s.rgb[2]);
                    }
                }
            }
            else
            {
                LogDebug("WARNING: Failed to parse sub-function %zu", i);
            }
        }

        // Pozisyona göre sırala
        std::sort(outStops.begin(), outStops.end(),
            [](const GradientStop& a, const GradientStop& b) {
                return a.position < b.position;
            });

        LogDebug("--- parseFunctionType3 END: total %zu stops ---", outStops.size());
        return !outStops.empty();
    }

} // namespace pdf