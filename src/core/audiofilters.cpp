/*
* Copyright (c) 2020 Fredrik Mellbin
*
* This file is part of VapourSynth.
*
* VapourSynth is free software; you can redistribute it and/or
* modify it under the terms of the GNU Lesser General Public
* License as published by the Free Software Foundation; either
* version 2.1 of the License, or (at your option) any later version.
*
* VapourSynth is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
* Lesser General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public
* License along with VapourSynth; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
*/

#include "internalfilters.h"
#include "VSHelper.h"
#include "filtershared.h"
#include "filtersharedcpp.h"

#include <cstdlib>
#include <cstdio>
#include <cinttypes>
#include <memory>
#include <limits>
#include <string>
#include <algorithm>
#include <vector>
#include <set>
#include <bitset>

/////////////
// TODO:
// channels_out should probably be a list in order to not be exceptionally confusing in shufflechannels and audiomix
// make channels_in for shufflechannels also accept negative numbers as a first, second and so on defined track to make certain uses easier
// improve audiosplice implementation to combine all clips at once instead of simply combining two at a time
// improve memory access pattern in audiomix, processing input and output in blocks of a few thousand samples should lead to much better cache locality
// implement audioloop filter
// implement audioreverse
// implement wavsource filter


//////////////////////////////////////////
// AudioTrim

typedef struct {
    VSNodeRef *node;
    VSAudioInfo ai;
    int64_t first;
} AudioTrimData;

static const VSFrameRef *VS_CC audioTrimGetframe(int n, int activationReason, void **instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    AudioTrimData *d = reinterpret_cast<AudioTrimData *>(*instanceData);

    int64_t startSample = n * (int64_t)d->ai.format->samplesPerFrame + d->first;
    int startFrame = (int)(startSample / d->ai.format->samplesPerFrame);
    int length = std::min<int>(d->ai.numSamples - n * static_cast<int64_t>(d->ai.format->samplesPerFrame), d->ai.format->samplesPerFrame);

    if (startSample % d->ai.format->samplesPerFrame == 0 && n != d->ai.numFrames - 1) { // pass through audio frames when possible
        if (activationReason == arInitial) {
            vsapi->requestFrameFilter(startFrame, d->node, frameCtx);
        } else if (activationReason == arAllFramesReady) {
            const VSFrameRef *src = vsapi->getFrameFilter(startFrame, d->node, frameCtx);
            if (length == vsapi->getFrameLength(src))
                return src;
            VSFrameRef *dst = vsapi->newAudioFrame(d->ai.format, d->ai.sampleRate, length, src, core);
            for (int channel = 0; channel < d->ai.format->numChannels; channel++)
                memcpy(vsapi->getWritePtr(dst, channel), vsapi->getReadPtr(src, channel), length * d->ai.format->bytesPerSample);
            vsapi->freeFrame(src);
            return dst;
        }
    } else {
        int numSrc1Samples = d->ai.format->samplesPerFrame - (startSample % d->ai.format->samplesPerFrame);
        if (activationReason == arInitial) {
            vsapi->requestFrameFilter(startFrame, d->node, frameCtx);
            if (numSrc1Samples < length)
                vsapi->requestFrameFilter(startFrame + 1, d->node, frameCtx);
        } else if (activationReason == arAllFramesReady) {
            const VSFrameRef *src1 = vsapi->getFrameFilter(startFrame, d->node, frameCtx);
            VSFrameRef *dst = vsapi->newAudioFrame(d->ai.format, d->ai.sampleRate, length, src1, core);
            for (int channel = 0; channel < d->ai.format->numChannels; channel++)
                memcpy(vsapi->getWritePtr(dst, channel), vsapi->getReadPtr(src1, channel) + (d->ai.format->samplesPerFrame - numSrc1Samples) * d->ai.format->bytesPerSample, numSrc1Samples * d->ai.format->bytesPerSample);
            vsapi->freeFrame(src1);

            if (length > numSrc1Samples) {
                const VSFrameRef *src2 = vsapi->getFrameFilter(startFrame + 1, d->node, frameCtx);
                for (int channel = 0; channel < d->ai.format->numChannels; channel++)
                    memcpy(vsapi->getWritePtr(dst, channel) + numSrc1Samples * d->ai.format->bytesPerSample, vsapi->getReadPtr(src2, channel), (length - numSrc1Samples) * d->ai.format->bytesPerSample);
                vsapi->freeFrame(src2);
            }

            return dst;
        }
    }

    return 0;
}

static void VS_CC audioTrimCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    std::unique_ptr<AudioTrimData> d(new AudioTrimData());

    int err;
    int64_t trimlen;

    d->first = int64ToIntS(vsapi->propGetInt(in, "first", 0, &err));
    bool firstset = !err;
    int64_t last = int64ToIntS(vsapi->propGetInt(in, "last", 0, &err));
    bool lastset = !err;
    int64_t length = int64ToIntS(vsapi->propGetInt(in, "length", 0, &err));
    bool lengthset = !err;

    if (lastset && lengthset)
        RETERROR("AudioTrim: both last sample and length specified");

    if (lastset && last < d->first)
        RETERROR("AudioTrim: invalid last sample specified (last is less than first)");

    if (lengthset && length < 1)
        RETERROR("AudioTrim: invalid length specified (less than 1)");

    if (d->first < 0)
        RETERROR("Trim: invalid first frame specified (less than 0)");

    d->node = vsapi->propGetNode(in, "clip", 0, 0);

    d->ai = *vsapi->getAudioInfo(d->node);

    if ((lastset && last >= d->ai.numSamples) || (lengthset && (d->first + length) > d->ai.numSamples) || (d->ai.numSamples <= d->first)) {
        vsapi->freeNode(d->node);
        RETERROR("AudioTrim: last sample beyond clip end");
    }

    if (lastset) {
        trimlen = last - d->first + 1;
    } else if (lengthset) {
        trimlen = length;
    } else {
        trimlen = d->ai.numSamples - d->first;
    }

    // obvious nop() so just pass through the input clip
    if ((!firstset && !lastset && !lengthset) || (trimlen && trimlen == d->ai.numSamples)) {
        vsapi->propSetNode(out, "clip", d->node, paReplace);
        vsapi->freeNode(d->node);
        return;
    }

    d->ai.numSamples = trimlen;

    vsapi->createAudioFilter(out, "AudioTrim", &d->ai, 1, audioTrimGetframe, templateNodeFree<AudioTrimData>, fmParallel, nfNoCache, d.get(), core);
    d.release();
}

//////////////////////////////////////////
// AudioSplice2 (can only combine two audio clips)

typedef struct {
    VSAudioInfo ai;
    VSNodeRef *node1;
    VSNodeRef *node2;
    int64_t numSamples1;
    int64_t numSamples2;
    int numFrames1;
} AudioSplice2Data;

static const VSFrameRef *VS_CC audioSplice2PassthroughGetframe(int n, int activationReason, void **instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    AudioSplice2Data *d = reinterpret_cast<AudioSplice2Data *>(*instanceData);
    if (activationReason == arInitial) {
        if (n < d->numFrames1)
            vsapi->requestFrameFilter(n, d->node1, frameCtx);
        else
            vsapi->requestFrameFilter(n - d->numFrames1, d->node2, frameCtx);
    } else if (activationReason == arAllFramesReady) {
        if (n < d->numFrames1)
            return vsapi->getFrameFilter(n, d->node1, frameCtx);
        else
            return vsapi->getFrameFilter(n - d->numFrames1, d->node2, frameCtx);
    }

    return 0;
}

static const VSFrameRef *VS_CC audioSplice2Getframe(int n, int activationReason, void **instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    AudioSplice2Data *d = reinterpret_cast<AudioSplice2Data *>(*instanceData);

    if (activationReason == arInitial) {
        if (n < d->numFrames1 - 1) {
            vsapi->requestFrameFilter(n, d->node1, frameCtx);
        } else if (n == d->numFrames1 - 1) {
            vsapi->requestFrameFilter(n, d->node1, frameCtx);
            vsapi->requestFrameFilter(0, d->node2, frameCtx);
        } else {
            vsapi->requestFrameFilter(n - d->numFrames1, d->node2, frameCtx);
            vsapi->requestFrameFilter(n - d->numFrames1 + 1, d->node2, frameCtx);
        }
    } else if (activationReason == arAllFramesReady) {
        const VSFrameRef *f1 = NULL;
        const VSFrameRef *f2 = NULL;

        if (n < d->numFrames1 - 1) {
            return vsapi->getFrameFilter(n, d->node1, frameCtx);
        } else if (n == d->numFrames1 - 1) {
            f1 = vsapi->getFrameFilter(n, d->node1, frameCtx);
            f2 = vsapi->getFrameFilter(0, d->node2, frameCtx);
        } else {
            f1 = vsapi->getFrameFilter(n - d->numFrames1, d->node2, frameCtx);
            f2 = vsapi->getFrameFilter(n - d->numFrames1 + 1, d->node2, frameCtx);
        }

        int samplesOut = std::min<int>(d->ai.format->samplesPerFrame, d->ai.numSamples - n * static_cast<int64_t>(d->ai.format->samplesPerFrame));

        VSFrameRef *f = vsapi->newAudioFrame(d->ai.format, d->ai.sampleRate, samplesOut, f1, core);

        //////////////

        if (n == d->numFrames1 - 1) {
            // handle the seam between clip 1 and 2 
            int f1copy = std::min(samplesOut, vsapi->getFrameLength(f1));
            int f2copy = samplesOut - f1copy;
            f1copy *= d->ai.format->bytesPerSample;
            f2copy *= d->ai.format->bytesPerSample;

            for (int channel = 0; channel < d->ai.format->numChannels; channel++) {
                memcpy(vsapi->getWritePtr(f, channel), vsapi->getReadPtr(f1, channel), f1copy);
                memcpy(vsapi->getWritePtr(f, channel) + f1copy, vsapi->getReadPtr(f2, channel), f2copy);
            }
        } else {
            int f1offset = d->ai.format->samplesPerFrame - ((d->numSamples1 - 1) % d->ai.format->samplesPerFrame) - 1;
            int f1copy = std::min(samplesOut, vsapi->getFrameLength(f1) - f1offset);
            int f2copy = samplesOut - f1copy;
            assert(f1copy > 0 && (f2copy > 0 || (f2copy >= 0 && n == d->ai.numFrames - 1)));
            f1copy *= d->ai.format->bytesPerSample;
            f2copy *= d->ai.format->bytesPerSample;
            f1offset *= d->ai.format->bytesPerSample;

            for (int channel = 0; channel < d->ai.format->numChannels; channel++) {
                memcpy(vsapi->getWritePtr(f, channel), vsapi->getReadPtr(f1, channel) + f1offset, f1copy);
                memcpy(vsapi->getWritePtr(f, channel) + f1copy, vsapi->getReadPtr(f2, channel), f2copy);
            }
        }

        ////////////////////

        vsapi->freeFrame(f1);
        vsapi->freeFrame(f2);

        return f;
    }

    return 0;
}

static void VS_CC audioSplice2Free(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    AudioSplice2Data *d = (AudioSplice2Data *)instanceData;
    vsapi->freeNode(d->node1);
    vsapi->freeNode(d->node2);
    delete d;
}

static void audioSplice2Create(VSNodeRef *clip1, VSNodeRef *clip2, VSMap *out, VSCore *core, const VSAPI *vsapi) {
    std::unique_ptr<AudioSplice2Data> d(new AudioSplice2Data);

    d->node1 = vsapi->cloneNodeRef(clip1);
    d->node2 = vsapi->cloneNodeRef(clip2);
    const VSAudioInfo *ai1 = vsapi->getAudioInfo(d->node1);
    const VSAudioInfo *ai2 = vsapi->getAudioInfo(d->node2);

    d->numFrames1 = ai1->numFrames;

    d->numSamples1 = ai1->numSamples;
    d->numSamples2 = ai2->numSamples;

    if (!isSameAudioFormat(ai1, ai2)) {
        vsapi->freeNode(d->node1);
        vsapi->freeNode(d->node2);
        RETERROR("AudioSplice: format mismatch");
    }

    d->ai = *ai1;
    d->ai.numSamples += d->numSamples2;
    d->ai.numFrames = (d->ai.numSamples + d->ai.format->samplesPerFrame - 1) / d->ai.format->samplesPerFrame;

    if (d->ai.numSamples < d->numSamples1 || d->ai.numSamples < d->numSamples2) {
        vsapi->freeNode(d->node1);
        vsapi->freeNode(d->node2);
        RETERROR("AudioSplice: the resulting clip is too long");
    }

    vsapi->createAudioFilter(out, "AudioSplice", &d->ai, 1, (d->numSamples1 % d->ai.format->samplesPerFrame) ? audioSplice2Getframe : audioSplice2PassthroughGetframe, audioSplice2Free, fmParallel, nfNoCache, d.get(), core);
    d.release();
}

//////////////////////////////////////////
// AudioSplice2Wrapper

static void VS_CC audioSplice2Wrapper(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    int numnodes = vsapi->propNumElements(in, "clips");

    if (numnodes == 1) { // passthrough for the special case with only one clip
        VSNodeRef *cref = vsapi->propGetNode(in, "clips", 0, 0);
        vsapi->propSetNode(out, "clip", cref, paReplace);
        vsapi->freeNode(cref);
    }

    VSNodeRef *tmp = vsapi->propGetNode(in, "clips", 0, 0);
    VSPlugin *plugin = vsapi->getPluginById("com.vapoursynth.std", core);

    for (int i = 1; i < numnodes; i++) {
        VSNodeRef *cref = vsapi->propGetNode(in, "clips", i, 0);
        audioSplice2Create(tmp, cref, out, core, vsapi);
        vsapi->freeNode(tmp);
        vsapi->freeNode(cref);

        if (vsapi->getError(out))
            return;

        tmp = vsapi->propGetNode(out, "clip", 0, 0);
        vsapi->clearMap(out);
    }

    vsapi->propSetNode(out, "clip", tmp, paReplace);
    vsapi->freeNode(tmp);
}

//////////////////////////////////////////
// AudioMix

struct AudioMixDataNode {
    VSNodeRef *node;
    int numFrames;
    int idx;
    std::vector<float> weights;
};

struct AudioMixData {
    std::vector<VSNodeRef *> reqNodes; // a list of all distinct nodes in sourceNodes to reduce function calls
    std::vector<AudioMixDataNode> sourceNodes;
    VSAudioInfo ai;
};

template<typename T>
static const VSFrameRef *VS_CC audioMixGetFrame(int n, int activationReason, void **instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    AudioMixData *d = reinterpret_cast<AudioMixData *>(*instanceData);

    if (activationReason == arInitial) {
        for (const auto &iter : d->reqNodes)
            vsapi->requestFrameFilter(n, iter, frameCtx);
    } else if (activationReason == arAllFramesReady) {     
        int numOutChannels = d->ai.format->numChannels;
        std::vector<const T *> srcPtrs;
        std::vector<const VSFrameRef *> srcFrames;
        srcPtrs.reserve(d->sourceNodes.size());
        srcFrames.reserve(d->sourceNodes.size());
        for (size_t idx = 0; idx < d->sourceNodes.size(); idx++) {
            const VSFrameRef *src = vsapi->getFrameFilter(n, d->sourceNodes[idx].node, frameCtx);                
            srcPtrs.push_back(reinterpret_cast<const T *>(vsapi->getReadPtr(src, d->sourceNodes[idx].idx)));
            srcFrames.push_back(src);
        }

        int srcLength = vsapi->getFrameLength(srcFrames[0]);
        VSFrameRef *dst = vsapi->newAudioFrame(d->ai.format, d->ai.sampleRate, srcLength, srcFrames[0], core);

        std::vector<T *> dstPtrs;
        dstPtrs.reserve(d->sourceNodes.size());
        for (int idx = 0; idx < numOutChannels; idx++)
            dstPtrs.push_back(reinterpret_cast<T *>(vsapi->getWritePtr(dst, idx)));

        for (int i = 0; i < srcLength; i++) {
            for (size_t dstIdx = 0; dstIdx < numOutChannels; dstIdx++) {
                double tmp = 0;
                for (size_t srcIdx = 0; srcIdx < srcPtrs.size(); srcIdx++)
                    tmp += static_cast<double>(srcPtrs[srcIdx][i]) * d->sourceNodes[srcIdx].weights[dstIdx];

                if (std::numeric_limits<T>::is_integer) {
                    if (sizeof(T) == 2) {
                        dstPtrs[dstIdx][i] = static_cast<int16_t>(tmp);
                    } else if (sizeof(T) == 4) {
                        tmp = std::min<float>(tmp, (static_cast<int64_t>(1) << (d->ai.format->bitsPerSample - 1)) - 1);
                        dstPtrs[dstIdx][i] = static_cast<int32_t>(tmp);
                    }
                } else {
                    dstPtrs[dstIdx][i] = tmp;
                }
            }
        }

        for (auto iter : srcFrames)
            vsapi->freeFrame(iter);

        return dst;
    }

    return nullptr;
}

static void VS_CC audioMixFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    AudioMixData *d = reinterpret_cast<AudioMixData *>(instanceData);
    for (const auto iter : d->sourceNodes)
        vsapi->freeNode(iter.node);
    delete d;
}

static void VS_CC audioMixCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    std::unique_ptr<AudioMixData> d(new AudioMixData());
    int numSrcNodes = vsapi->propNumElements(in, "clips");
    int numMatrixWeights = vsapi->propNumElements(in, "matrix");
    int64_t channels_out = vsapi->propGetInt(in, "channels_out", 0, nullptr);

    std::bitset<64> tmp(channels_out);
    int numOutChannels = tmp.count();
    int numSrcChannels = 0;

    for (int i = 0; i < numSrcNodes; i++) {
        VSNodeRef *node = vsapi->propGetNode(in, "clips", std::min(numSrcNodes - 1, i), nullptr);
        const VSAudioFormat *f = vsapi->getAudioInfo(node)->format;
        for (int j = 0; j < f->numChannels; j++) {
            d->sourceNodes.push_back({ (j > 0) ?  vsapi->cloneNodeRef(node) : node, j });
            numSrcChannels++;
        }
    }

    if (numSrcNodes > numSrcChannels) {
        vsapi->setError(out, "AudioMix: cannot have more input nodes than selected input channels");
        for (const auto iter : d->sourceNodes)
            vsapi->freeNode(iter.node);
        return;
    }

    if (numOutChannels * numSrcChannels != numMatrixWeights) {
        vsapi->setError(out, "AudioMix: the number of matrix weights must equal (input channels * output channels)");
        for (const auto iter : d->sourceNodes)
            vsapi->freeNode(iter.node);
        return;
    }

    const char *err = nullptr;

    d->ai = *vsapi->getAudioInfo(d->sourceNodes[0].node);
    for (size_t i = 0; i < d->sourceNodes.size(); i++) {
        const VSAudioInfo *ai = vsapi->getAudioInfo(d->sourceNodes[i].node);
        if (!(ai->format->channelLayout & (static_cast<int64_t>(1) << d->sourceNodes[i].idx))) {
            err = "AudioMix: specified channel is not present in input";
            break;
        }
        if (ai->numSamples != d->ai.numSamples || ai->sampleRate != d->ai.sampleRate || ai->format->bitsPerSample != d->ai.format->bitsPerSample || ai->format->sampleType != d->ai.format->sampleType) {
            err = "AudioMix: all inputs must have the same length, samplerate, bits per sample and sample type";
            break;
        }
        // recalculate channel number to a simple index (add as a vsapi function?)
        int idx = 0;
        for (int j = 0; j < d->sourceNodes[i].idx; j++)
            if (ai->format->channelLayout & (static_cast<int64_t>(1) << j))
                idx++;
        d->ai.numSamples = std::max(d->ai.numSamples, ai->numSamples);
        for (int j = 0; j < numOutChannels; j++)
            d->sourceNodes[i].weights.push_back(vsapi->propGetFloat(in, "matrix", j * numSrcChannels + i, nullptr));
        d->sourceNodes[i].numFrames = ai->numFrames;
        d->sourceNodes[i].idx = idx;
    }

    d->ai.format = vsapi->queryAudioFormat(d->ai.format->sampleType, d->ai.format->bitsPerSample, channels_out, core);
    if (!d->ai.format) {
        err = "AudioMix: invalid output channnel configuration";
    }

    if (err) {
        vsapi->setError(out, err);
        for (const auto &iter : d->sourceNodes)
            vsapi->freeNode(iter.node);
        return;
    }

    std::set<VSNodeRef *> nodeSet;

    for (const auto &iter : d->sourceNodes)
        nodeSet.insert(iter.node);

    for (const auto &iter : nodeSet)
        d->reqNodes.push_back(iter);

    if (d->ai.format->sampleType == stFloat)
        vsapi->createAudioFilter(out, "AudioMix", &d->ai, 1, audioMixGetFrame<float>, audioMixFree, fmParallel, 0, d.get(), core);
    else if (d->ai.format->bytesPerSample == 2)
        vsapi->createAudioFilter(out, "AudioMix", &d->ai, 1, audioMixGetFrame<int16_t>, audioMixFree, fmParallel, 0, d.get(), core);
    else
        vsapi->createAudioFilter(out, "AudioMix", &d->ai, 1, audioMixGetFrame<int32_t>, audioMixFree, fmParallel, 0, d.get(), core);
    d.release();
}

//////////////////////////////////////////
// ShuffleChannels

struct ShuffleChannelsDataNode {
    VSNodeRef *node;
    int numFrames;
    int idx;
};

struct ShuffleChannelsData {
    std::vector<VSNodeRef *> reqNodes; // a list of all distinct nodes in sourceNodes to reduce function calls
    std::vector<ShuffleChannelsDataNode> sourceNodes;
    VSAudioInfo ai;
};

static const VSFrameRef *VS_CC shuffleChannelsGetFrame(int n, int activationReason, void **instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    ShuffleChannelsData *d = reinterpret_cast<ShuffleChannelsData *>(*instanceData);

    if (activationReason == arInitial) {
        for (const auto &iter : d->reqNodes)
            vsapi->requestFrameFilter(n, iter, frameCtx);
    } else if (activationReason == arAllFramesReady) {
        VSFrameRef *dst = nullptr;
        int dstLength = std::min<int>(d->ai.numSamples - n * static_cast<int64_t>(d->ai.format->samplesPerFrame), d->ai.format->samplesPerFrame);
        for (size_t idx = 0; idx < d->sourceNodes.size(); idx++) {
            const VSFrameRef *src = vsapi->getFrameFilter(n, d->sourceNodes[idx].node, frameCtx);;
            int srcLength = (n < d->sourceNodes[idx].numFrames) ? vsapi->getFrameLength(src) : 0;
            int copyLength = std::min(dstLength, srcLength);
            int zeroLength = dstLength - copyLength;
            if (!dst)
                dst = vsapi->newAudioFrame(d->ai.format, d->ai.sampleRate, dstLength, src, core);
            if (copyLength > 0)
                memcpy(vsapi->getWritePtr(dst, idx), vsapi->getReadPtr(src, d->sourceNodes[idx].idx), copyLength * d->ai.format->bytesPerSample);
            if (zeroLength > 0)
                memset(vsapi->getWritePtr(dst, idx) + copyLength * d->ai.format->bytesPerSample, 0, zeroLength * d->ai.format->bytesPerSample);
            vsapi->freeFrame(src);
        }

        return dst;
    }

    return nullptr;
}

static void VS_CC shuffleChannelsFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    ShuffleChannelsData *d = reinterpret_cast<ShuffleChannelsData *>(instanceData);
    for (const auto iter : d->sourceNodes)
        vsapi->freeNode(iter.node);
    delete d;
}

static void VS_CC shuffleChannelsCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    std::unique_ptr<ShuffleChannelsData> d(new ShuffleChannelsData());
    int numSrcNodes = vsapi->propNumElements(in, "clip");
    int numSrcChannels = vsapi->propNumElements(in, "channels_in");
    int64_t channels_out = vsapi->propGetInt(in, "channels_out", 0, nullptr);

    if (numSrcNodes > numSrcChannels) {
        vsapi->setError(out, "ShuffleChannels: cannot have more input nodes than selected input channels");
        return;
    }

    std::bitset<64> tmp(channels_out);
    if (tmp.count() != numSrcChannels) {
        vsapi->setError(out, "ShuffleChannels: number of input channels doesn't match number of outputs");
        return;
    }

    for (int i = 0; i < numSrcChannels; i++) {
        int channel = int64ToIntS(vsapi->propGetInt(in, "channels_in", i, nullptr));
        VSNodeRef *node = vsapi->propGetNode(in, "clip", std::min(numSrcNodes - 1, i), nullptr);
        d->sourceNodes.push_back({ node, channel });
    }

    const char *err = nullptr;

    d->ai = *vsapi->getAudioInfo(d->sourceNodes[0].node);
    for (size_t i = 0; i < d->sourceNodes.size(); i++) {
        const VSAudioInfo *ai = vsapi->getAudioInfo(d->sourceNodes[i].node);
        if (!(ai->format->channelLayout & (static_cast<int64_t>(1) << d->sourceNodes[i].idx))) {
            err = "ShuffleChannels: specified channel is not present in input";
            break;
        }
        if (ai->sampleRate != d->ai.sampleRate || ai->format->bitsPerSample != d->ai.format->bitsPerSample || ai->format->sampleType != d->ai.format->sampleType) {
            err = "ShuffleChannels: all inputs must have the same samplerate, bits per sample and sample type";
            break;
        }
        // recalculate channel number to a simple index (add as a vsapi function?)
        int idx = 0;
        for (int j = 0; j < d->sourceNodes[i].idx; j++)
            if (ai->format->channelLayout & (static_cast<int64_t>(1) << j))
                idx++;
        d->ai.numSamples = std::max(d->ai.numSamples, ai->numSamples);
        d->sourceNodes[i].numFrames = ai->numFrames;
        d->sourceNodes[i].idx = idx;
    }

    d->ai.format = vsapi->queryAudioFormat(d->ai.format->sampleType, d->ai.format->bitsPerSample, channels_out, core);
    if (!d->ai.format) {
        err = "ShuffleChannels: invalid output channnel configuration";
    }

    if (err) {
        vsapi->setError(out, err);
        for (const auto &iter : d->sourceNodes)
            vsapi->freeNode(iter.node);
        return;
    }

    std::set<VSNodeRef *> nodeSet;

    for (const auto &iter : d->sourceNodes)
        nodeSet.insert(iter.node);

    for (const auto &iter : nodeSet)
        d->reqNodes.push_back(iter);

    vsapi->createAudioFilter(out, "ShuffleChannels", &d->ai, 1, shuffleChannelsGetFrame, shuffleChannelsFree, fmParallel, 0, d.get(), core);
    d.release();
}

//////////////////////////////////////////
// SplitChannels

struct SplitChannelsData {
    std::vector<VSAudioInfo> ai;
    VSNodeRef *node;
    int numChannels;
};

static const VSFrameRef *VS_CC splitChannelsGetFrame(int n, int activationReason, void **instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    SplitChannelsData *d = reinterpret_cast<SplitChannelsData *>(*instanceData);

    if (activationReason == arInitial) {
        vsapi->requestFrameFilter(n, d->node, frameCtx);
    } else if (activationReason == arAllFramesReady) {
        const VSFrameRef *src = vsapi->getFrameFilter(n, d->node, frameCtx);
        int outIdx = vsapi->getOutputIndex(frameCtx);
        int length = vsapi->getFrameLength(src);
        VSFrameRef *dst = vsapi->newAudioFrame(d->ai[outIdx].format, d->ai[outIdx].sampleRate, length, src, core);
        memcpy(vsapi->getWritePtr(dst, 0), vsapi->getReadPtr(src, outIdx), d->ai[outIdx].format->bytesPerSample * length);
        vsapi->freeFrame(src);
        return dst;
    }

    return nullptr;
}

static void VS_CC splitChannelsFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    SplitChannelsData *d = reinterpret_cast<SplitChannelsData *>(instanceData);
    vsapi->freeNode(d->node);
    delete d;
}

static void VS_CC splitChannelsCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    std::unique_ptr<SplitChannelsData> d(new SplitChannelsData());
    d->node = vsapi->propGetNode(in, "clip", 0, nullptr);
    VSAudioInfo ai = *vsapi->getAudioInfo(d->node);
    d->numChannels = ai.format->numChannels;
    d->ai.reserve(d->numChannels);
    for (int i = 0; i < d->numChannels; i++) {
        // fixme, preserve actual channel here?
        ai.format = vsapi->queryAudioFormat(ai.format->sampleType, ai.format->bitsPerSample, (1 << vsacFrontLeft), core);
        d->ai.push_back(ai);
    }

    vsapi->createAudioFilter(out, "SplitChannels", d->ai.data(), d->numChannels, splitChannelsGetFrame, splitChannelsFree, fmParallel, 0, d.get(), core);
    d.release();
}

//////////////////////////////////////////
// AssumeSampleRate

typedef struct {
    VSNodeRef *node;
} AssumeSampleRateData;

static const VSFrameRef *VS_CC assumeSampleRateGetframe(int n, int activationReason, void **instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    AssumeSampleRateData *d = reinterpret_cast<AssumeSampleRateData *>(*instanceData);

    if (activationReason == arInitial) {
        vsapi->requestFrameFilter(n, d->node, frameCtx);
    } else if (activationReason == arAllFramesReady) {
        return vsapi->getFrameFilter(n, d->node, frameCtx);
    }

    return 0;
}

static void VS_CC assumeSampleRateCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    std::unique_ptr<AssumeSampleRateData> d(new AssumeSampleRateData());
    bool hassamplerate = false;
    bool hassrc = false;
    int err;

    d->node = vsapi->propGetNode(in, "clip", 0, 0);
    VSAudioInfo ai = *vsapi->getAudioInfo(d->node);

    ai.sampleRate = int64ToIntS(vsapi->propGetInt(in, "samplerate", 0, &err));
    if (!err)
        hassamplerate = true;

    VSNodeRef *src = vsapi->propGetNode(in, "src", 0, &err);

    if (!err) {
        const VSVideoInfo *vi = vsapi->getVideoInfo(src);
        ai.sampleRate = vsapi->getAudioInfo(d->node)->sampleRate;
        vsapi->freeNode(src);
        hassrc = true;
    }

    if (hassamplerate == hassrc) {
        vsapi->freeNode(d->node);
        RETERROR("AssumeSampleRate: need to specify source clip or samplerate");
    }

    if (ai.sampleRate < 1) {
        vsapi->freeNode(d->node);
        RETERROR("AssumeSampleRate: invalid samplerate specified");
    }

    vsapi->createAudioFilter(out, "AssumeSampleRate", &ai, 1, assumeSampleRateGetframe, templateNodeFree<AssumeSampleRateData>, fmParallel, nfNoCache, d.get(), core);
    d.release();
}

//////////////////////////////////////////
// BlankAudio

typedef struct {
    VSFrameRef *f;
    VSAudioInfo ai;
    bool keep;
} BlankAudioData;

static const VSFrameRef *VS_CC blankAudioGetframe(int n, int activationReason, void **instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    BlankAudioData *d = reinterpret_cast<BlankAudioData *>(*instanceData);

    if (activationReason == arInitial) {
        VSFrameRef *frame = NULL;
        if (!d->f) {
            int samples = std::min<int>(d->ai.format->samplesPerFrame, d->ai.numSamples - n * (int64_t)d->ai.format->samplesPerFrame);
            frame = vsapi->newAudioFrame(d->ai.format, d->ai.sampleRate, samples, NULL, core);
            for (int channel = 0; channel < d->ai.format->numChannels; channel++)
                memset(vsapi->getWritePtr(frame, channel), 0, samples * d->ai.format->bytesPerSample);
        }

        if (d->keep) {
            if (frame)
                d->f = frame;
            return vsapi->cloneFrameRef(d->f);
        } else {
            return frame;
        }
    }

    return 0;
}

static void VS_CC blankAudioFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    BlankAudioData *d = reinterpret_cast<BlankAudioData *>(instanceData);
    vsapi->freeFrame(d->f);
    delete d;
}

static void VS_CC blankAudioCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    std::unique_ptr<BlankAudioData> d(new BlankAudioData());

    int err;

    int64_t channels = vsapi->propGetInt(in, "channels", 0, &err);
    if (err)
        channels = (1 << vsacFrontLeft) | (1 << vsacFrontRight);

    int bits = int64ToIntS(vsapi->propGetInt(in, "bits", 0, &err));
    if (err)
        bits = 16;

    bool isfloat = !!vsapi->propGetInt(in, "isfloat", 0, &err);

    d->keep = !!vsapi->propGetInt(in, "keep", 0, &err);

    d->ai.sampleRate = int64ToIntS(vsapi->propGetInt(in, "samplerate", 0, &err));
    if (err)
        d->ai.sampleRate = 44100;

    d->ai.numSamples = vsapi->propGetInt(in, "length", 0, &err);
    if (err)
        d->ai.numSamples = static_cast<int64_t>(d->ai.sampleRate) * 60 * 60;

    if (d->ai.sampleRate <= 0)
        RETERROR("BlankAudio: invalid sample rate");

    if (d->ai.numSamples <= 0)
        RETERROR("BlankAudio: invalid length");

    d->ai.format = vsapi->queryAudioFormat(isfloat ? stFloat : stInteger, bits, channels, core);

    if (!d->ai.format)
        RETERROR("BlankAudio: invalid format");

    vsapi->createAudioFilter(out, "BlankAudio", &d->ai, 1, blankAudioGetframe, blankAudioFree, d->keep ? fmUnordered : fmParallel, nfNoCache, d.get(), core);
    d.release();
}

//////////////////////////////////////////
// TestAudio

typedef struct {
    VSAudioInfo ai;
} TestAudioData;

static const VSFrameRef *VS_CC testAudioGetframe(int n, int activationReason, void **instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    TestAudioData *d = reinterpret_cast<TestAudioData *>(*instanceData);

    if (activationReason == arInitial) {
        int samples = std::min<int>(d->ai.format->samplesPerFrame, d->ai.numSamples - n * static_cast<int64_t>(d->ai.format->samplesPerFrame));
        int64_t startSample = n * (int64_t)d->ai.format->samplesPerFrame;
        VSFrameRef *frame = vsapi->newAudioFrame(d->ai.format, d->ai.sampleRate, samples, NULL, core);
        for (int channel = 0; channel < d->ai.format->numChannels; channel++) {
            uint16_t *w = reinterpret_cast<uint16_t *>(vsapi->getWritePtr(frame, channel));
            for (int i = 0; i < samples; i++)
                w[i] = (startSample + i) % 0xFFFF;
        }
        return frame;
    }

    return 0;
}

static void VS_CC testAudioFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    TestAudioData *d = reinterpret_cast<TestAudioData *>(instanceData);
    delete d;
}

static void VS_CC testAudioCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    std::unique_ptr<TestAudioData> d(new TestAudioData());

    int err;

    int64_t channels = vsapi->propGetInt(in, "channels", 0, &err);
    if (err)
        channels = (1 << vsacFrontLeft) | (1 << vsacFrontRight);

    int bits = int64ToIntS(vsapi->propGetInt(in, "bits", 0, &err));
    if (err)
        bits = 16;

    if (bits != 16)
        RETERROR("TestAudio: bits must be 16!");

    bool isfloat = !!vsapi->propGetInt(in, "isfloat", 0, &err);

    d->ai.sampleRate = int64ToIntS(vsapi->propGetInt(in, "samplerate", 0, &err));
    if (err)
        d->ai.sampleRate = 44100;

    d->ai.numSamples = vsapi->propGetInt(in, "length", 0, &err);
    if (err)
        d->ai.numSamples = static_cast<int64_t>(d->ai.sampleRate) * 60 * 60;

    if (d->ai.sampleRate <= 0)
        RETERROR("TestAudio: invalid sample rate");

    if (d->ai.numSamples <= 0)
        RETERROR("TestAudio: invalid length");

    d->ai.format = vsapi->queryAudioFormat(isfloat ? stFloat : stInteger, bits, channels, core);

    if (!d->ai.format)
        RETERROR("TestAudio: invalid format");

    vsapi->createAudioFilter(out, "TestAudio", &d->ai, 1, testAudioGetframe, testAudioFree, fmParallel, nfNoCache, d.get(), core);
    d.release();
}

//////////////////////////////////////////
// Init

void VS_CC audioInitialize(VSConfigPlugin configFunc, VSRegisterFunction registerFunc, VSPlugin *plugin) {
    //configFunc("com.vapoursynth.std", "std", "VapourSynth Core Functions", VAPOURSYNTH_API_VERSION, 1, plugin);
    registerFunc("AudioTrim", "clip:anode;first:int:opt;last:int:opt;length:int:opt;", audioTrimCreate, 0, plugin);
    registerFunc("AudioSplice", "clips:anode[];", audioSplice2Wrapper, 0, plugin);
    registerFunc("AudioMix", "clips:anode[];matrix:float[];channels_out:int;", audioMixCreate, 0, plugin);
    registerFunc("ShuffleChannels", "clip:anode[];channels_in:int[];channels_out:int;", shuffleChannelsCreate, 0, plugin);
    registerFunc("SplitChannels", "clip:anode;", splitChannelsCreate, 0, plugin);
    registerFunc("AssumeSampleRate", "clip:anode;src:anode:opt;samplerate:int:opt;", assumeSampleRateCreate, 0, plugin);
    registerFunc("BlankAudio", "channels:int:opt;bits:int:opt;isfloat:int:opt;samplerate:int:opt;length:int:opt;keep:int:opt;", blankAudioCreate, 0, plugin);
    registerFunc("TestAudio", "channels:int:opt;bits:int:opt;isfloat:int:opt;samplerate:int:opt;length:int:opt;", testAudioCreate, 0, plugin);
}
