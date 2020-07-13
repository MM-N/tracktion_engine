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

/**
    Sends an input Node to a Rack bus handling the channel mapping and channel gain levels.
*/
class RackInstanceNode final    : public tracktion_graph::Node
{
public:
    /** Creates a RackInstanceNode that maps an input node channel to an output channel
        and applies a gain parameter to each mapped channel.
    */
    RackInstanceNode (std::unique_ptr<Node>,
                      std::array<std::tuple<int, int, AutomatableParameter::Ptr>, 2> channelMap);

    std::vector<Node*> getDirectInputNodes() override;
    tracktion_graph::NodeProperties getNodeProperties() override;
    void prepareToPlay (const tracktion_graph::PlaybackInitialisationInfo&) override;
    bool isReadyToProcess() override;
    void process (const ProcessContext&) override;

private:
    //==============================================================================
    std::unique_ptr<Node> input;
    std::array<std::tuple<int, int, AutomatableParameter::Ptr>, 2> channelMap;
    int maxNumChannels = 0;
    float lastGain[2];
};

} // namespace tracktion_engine
