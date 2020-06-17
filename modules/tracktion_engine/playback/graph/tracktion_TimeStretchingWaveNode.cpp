/*
    ,--.                     ,--.     ,--.  ,--.
  ,-'  '-.,--.--.,--,--.,---.|  |,-.,-'  '-.`--' ,---. ,--,--,      Copyright 2018
  '-.  .-'|  .--' ,-.  | .--'|     /'-.  .-',--.| .-. ||      \   Tracktion Software
    |  |  |  |  \ '-'  \ `--.|  \  \  |  |  |  |' '-' '|  ||  |       Corporation
    `---' `--'   `--`--'`---'`--'`--' `---' `--' `---' `--''--'    www.tracktion.com

    Tracktion Engine uses a GPL/commercial licence - see LICENCE.md for details.
*/

namespace tracktion_engine
{

//==============================================================================
//==============================================================================
TimeStretchingWaveNode::TimeStretchingWaveNode (AudioClipBase& clip, tracktion_graph::PlayHeadState& playHeadStateToUse)
    : c (clip), playHeadState (playHeadStateToUse),
      file (c.getAudioFile()),
      fileInfo (file.getInfo()),
      sampleRate (fileInfo.sampleRate),
      fifo (std::max (1, fileInfo.numChannels), 8192)
{
    CRASH_TRACER

    reader = c.edit.engine.getAudioFileManager().cache.createReader (file);

    auto wi = clip.getWaveInfo();
    auto& li = c.getLoopInfo();

    if (li.getNumBeats() > 0)
    {
        if (c.getAutoTempo() && wi.hashCode != 0)
            speedRatio = (float) (li.getBpm (wi) / clip.edit.tempoSequence.getTempo (0)->getBpm());
        else
            speedRatio = (float) c.getSpeedRatio();
    }

    const int rootNote = li.getRootNote();

    if (rootNote != -1)
    {
        if (c.getAutoPitch() && rootNote != -1)
            pitchSemitones = (float) c.getTransposeSemiTones (true);
        else
            pitchSemitones = c.getPitchChange();
    }

    jassert (speedRatio >= 0.1f);
    speedRatio = std::max (speedRatio, 0.1f);
}

//==============================================================================
tracktion_graph::NodeProperties TimeStretchingWaveNode::getNodeProperties()
{
    tracktion_graph::NodeProperties props;
    props.hasAudio = true;
    props.numberOfChannels = fileInfo.numChannels;
    return props;
}

std::vector<tracktion_graph::Node*> TimeStretchingWaveNode::getDirectInputNodes()
{
    return {};
}

void TimeStretchingWaveNode::prepareToPlay (const tracktion_graph::PlaybackInitialisationInfo& info)
{
    CRASH_TRACER

    stretchBlockSize = std::max (info.blockSize, 512);
    sampleRate = info.sampleRate;

    const TimeStretcher::Mode m = c.getTimeStretchMode();

    if (TimeStretcher::canProcessFor (m))
    {
        fileSpeedRatio = float (fileInfo.sampleRate / sampleRate);
        const float resamplingPitchRatio = fileSpeedRatio > 0.0f ? (float) std::log2 (fileSpeedRatio) : 1.0f;
        timestretcher.initialise (info.sampleRate, stretchBlockSize, fileInfo.numChannels, m, c.elastiqueProOptions.get(), false);
        timestetchSpeedRatio = std::max (0.1f, float (speedRatio / fileSpeedRatio));
        timestetchSemitonesUp = float ((pitchSemitones + (resamplingPitchRatio * 12.0f)));
        timestretcher.setSpeedAndPitch (timestetchSpeedRatio, timestetchSemitonesUp);
    }

    fifo.setSize (fileInfo.numChannels, timestretcher.getMaxFramesNeeded());
}

bool TimeStretchingWaveNode::isReadyToProcess()
{
    if (file.getHash() == 0)
        return true;

    if (reader == nullptr)
        reader = c.edit.engine.getAudioFileManager().cache.createReader (file);

    return reader != nullptr && reader->getSampleRate() > 0.0;
}

void TimeStretchingWaveNode::process (const ProcessContext& pc)
{
    CRASH_TRACER
    const auto timelineRange = referenceSampleRangeToSplitTimelineRange (playHeadState.playHead, pc.referenceSampleRange).timelineRange1;
    const auto editRange = tracktion_graph::sampleToTime (timelineRange, sampleRate);

    auto destAudioBlock = pc.buffers.audio;

    if (! playHeadState.isContiguousWithPreviousBlock() || editRange.getStart() != nextEditTime)
        reset (editRange.getStart());

    auto numSamples = (int) destAudioBlock.getNumSamples();
    auto start = 0;

    while (numSamples > 0)
    {
        const int numReady = std::min (numSamples, fifo.getNumReady());

        if (numReady > 0)
        {
            const bool res = fifo.readAdding (destAudioBlock.getSubBlock ((size_t) start, (size_t) numReady));
            jassert (res); juce::ignoreUnused (res);

            start += numReady;
            numSamples -= numReady;
        }
        else
        {
            if (! fillNextBlock())
                break;
        }
    }

    nextEditTime = editRange.getEnd();
}

//==============================================================================
int64_t TimeStretchingWaveNode::timeToFileSample (double time) const noexcept
{
    const double fileStartTime = time / speedRatio;
    return juce::roundToInt (fileStartTime * fileInfo.sampleRate);
}

void TimeStretchingWaveNode::reset (double newStartTime)
{
    const int64_t readPos = timeToFileSample (newStartTime);

    if (reader != nullptr)
    {
        if (readPos == reader->getReadPosition())
            return;

        reader->setReadPosition (readPos);
    }

    fifo.reset();

    timestretcher.reset();
    timestretcher.setSpeedAndPitch (timestetchSpeedRatio, timestetchSemitonesUp);
}

bool TimeStretchingWaveNode::fillNextBlock()
{
    CRASH_TRACER
    const int needed = timestretcher.getFramesNeeded();
    jassert (needed < fifo.getFreeSpace());
    jassert (reader != nullptr);

    if (reader == nullptr)
        return false;

    AudioScratchBuffer fifoScratch (fileInfo.numChannels, stretchBlockSize);

    float* outs[] = { fifoScratch.buffer.getWritePointer (0),
        fileInfo.numChannels > 1 ? fifoScratch.buffer.getWritePointer (1) : nullptr,
        nullptr };

    if (needed >= 0)
    {
        AudioScratchBuffer scratch (fileInfo.numChannels, needed);
        const juce::AudioChannelSet bufChannels = fileInfo.numChannels == 1 ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo();
        const juce::AudioChannelSet channelsToUse = juce::AudioChannelSet::stereo();

        if (needed > 0)
        {
            bool b = reader->readSamples (needed, scratch.buffer, bufChannels, 0, channelsToUse, 3);
            juce::ignoreUnused (b);
            // don't worry about failed reads -- they are cache misses. It'll catch up
        }

        const float* ins[] = { scratch.buffer.getReadPointer (0),
            fileInfo.numChannels > 1 ? scratch.buffer.getReadPointer (1) : nullptr,
            nullptr
        };

        if (TimeStretcher::canProcessFor (c.getTimeStretchMode()))
            timestretcher.processData (ins, needed, outs);
        else
            for (int channel = fileInfo.numChannels; --channel >= 0;)
                juce::FloatVectorOperations::copy (outs[channel], ins[channel], needed);
    }
    else
    {
        jassert (needed == -1);
        timestretcher.flush (outs);
    }

    const bool res = fifo.write (fifoScratch.buffer);
    jassert (res); juce::ignoreUnused (res);

    return true;
}

} // namespace tracktion_engine
