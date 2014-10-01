#include <vector>

#include "DDImage/Writer.h"
#include "DDImage/Thread.h"
#include "DDImage/Row.h"

#include "OpenImageIO/filter.h"
#include "OpenImageIO/imagebuf.h"
#include "OpenImageIO/imagebufalgo.h"


/*
 * NOTES:
 *
 * maketx:oiio_options:
 * - configspec.attribute ("planarconfig", "contig")
 * - Sets tile size to 64x64
 *
 * maketx:prman_options:
 * - configspec.attribute ("planarconfig", "separate");
 * - configspec.attribute ("maketx:prman_metadata", 1);
 * - Controls bit depth and tile size
 *
 *
 * TODO:
 * - Re-enable "checknan" functionality when make_texture bug is fixed
 *      (uncomment knob code and default knob storage var to true).
 * - Look into using an ImageBuf iterator to fill the source buffer (either
 *      using buf[chan] = value or, less likely, *buf.rawptr() = value).
 * - Add support for output presets ("oiio", "prman", "custom")
 *      For prman, should also enable "maketx:prman_metadata"
 *      For "custom", should expose W/H tileSize override, planar config
 * - Either add support for more than 4 output channels, or make sure to clamp
 *      num_channels() in execute(). If we support more, we'll need to set
 *      output channel names.
 * - Could throw Nuke script name and/or tree hash into the metadata
 */


using namespace DD::Image;


namespace TxWriterNS
{


OIIO_NAMESPACE_USING

// Limit the available output datatypes (for now, at least).
static const TypeDesc::BASETYPE oiioBitDepths[] = {TypeDesc::INT8,
                                                   TypeDesc::INT16,
                                                   TypeDesc::INT32,
                                                   TypeDesc::FLOAT,
                                                   TypeDesc::DOUBLE};

// Labels for above bit depths (keep them synced!)
static const char* const bitDepthNames[] = {"8-bit integer",
                                            "16-bit integer",
                                            "32-bit integer",
                                            "32-bit float",
                                            "64-bit double",
                                            NULL};

bool gTxFiltersInitialized = false;
static std::vector<const char*> gFilterNames;


class txWriter : public Writer {
    int bitDepth_;
    int filter_;
    bool fixNan_;
    int nanFixType_;
    bool checkNan_;
    bool verbose_;
    bool stats_;

public:
    txWriter(Write* iop) : Writer(iop),
            bitDepth_(3),  // float
            filter_(0),
            fixNan_(false),
            nanFixType_(0),
            checkNan_(false),  // FIXME: Default to true when possible
            verbose_(false),
            stats_(false)
    {
        if (!gTxFiltersInitialized) {
            for (int i = 0, e = Filter2D::num_filters();  i < e;  ++i) {
                FilterDesc d;
                Filter2D::get_filterdesc (i, &d);
                gFilterNames.push_back(d.name);
            };
            gFilterNames.push_back(NULL);
            gTxFiltersInitialized = true;
        }
    }

    void knobs(Knob_Callback cb) {
        Enumeration_knob(cb, &bitDepth_, &bitDepthNames[0], "tx_datatype",
                         "datatype");
        Tooltip(cb, "The datatype of the output image.");

        Enumeration_knob(cb, &filter_, &gFilterNames[0], "tx_filter", "filter");
        Tooltip(cb, "The filter used to resize the image when generating mip "
                "levels.");

        Bool_knob(cb, &fixNan_, "fix_nan", "fix NaN/Inf pixels");
        Tooltip(cb, "Attempt to fix NaN/Inf pixel values in the image.");
        SetFlags(cb, Knob::STARTLINE);

        static const char* const fixLabels[] = {"black\tblack",
                                                "box3\tbox3 filter",
                                                NULL};
        Knob* k = Enumeration_knob(cb, &nanFixType_, &fixLabels[0],
                                   "nan_fix_type", "");
        if (cb.makeKnobs())
            k->disable();
        else if (fixNan_)
            k->enable();

        Tooltip(cb, "The method to use to fix NaN/Inf pixel values.");
        ClearFlags(cb, Knob::STARTLINE);

        // FIXME: Enable this after make_texture bug is fixed.
//        Bool_knob(cb, &checkNan_, "check_nan", "error on NaN/Inf");
//        Tooltip(cb, "Check for NaN/Inf pixel values in the output image, and "
//                "error if any are found. If this is enabled, the check will be "
//                "run <b>after</b> the NaN fix process.");
//        SetFlags(cb, Knob::STARTLINE);

        Bool_knob(cb, &verbose_, "verbose");
        Tooltip(cb, "Toggle verbose OIIO output.");
        SetFlags(cb, Knob::STARTLINE);

        Bool_knob(cb, &stats_, "oiio_stats", "output stats");
        Tooltip(cb, "Toggle output of OIIO runtime statistics.");
        ClearFlags(cb, Knob::STARTLINE);
    }

    int knob_changed(Knob* k) {
        printf("txWriter::knob_changed\n");
        if (k->is("fix_nan")) {
            iop->knob("nan_fix_type")->enable(fixNan_);
            return 1;
        }

        return Writer::knob_changed(k);
    }

    void execute() {
        int chanCount = num_channels();
        if (chanCount > 4) {
            iop->warning("Truncating output to 4 channels");
            chanCount = 4;
        }
        ChannelSet channels = channel_mask(chanCount);
        const bool doAlpha = channels.contains(Chan_Alpha);

        iop->progressMessage("Preparing image");
        input0().request(0, 0, width(), height(), channels, 1);

        if (aborted())
            return;

        ImageSpec srcSpec(width(), height(), chanCount, TypeDesc::FLOAT);
        ImageBuf srcBuffer(srcSpec);
        Row row(0, width());
        // Buffer for a channel-interleaved row after output LUT processing
        std::vector<float> lutBuffer(width() * chanCount);

        for (int y = 0; y < height(); y++) {
            iop->progressFraction(double(y) / height() * 0.85);
            get(height() - y - 1, 0, width(), channels, row);
            if (aborted())
                return;

            const float* alpha = doAlpha ? row[Chan_Alpha] : NULL;

            for (int i = 0; i < chanCount; i++)
                to_float(i, &lutBuffer[i], row[channel(i)], alpha, width(),
                         chanCount);
            for (int x = 0; x < width(); x++)
                srcBuffer.setpixel(x, y, &lutBuffer[x * chanCount]);
        }

        ImageSpec destSpec(width(), height(), chanCount, oiioBitDepths[bitDepth_]);

        destSpec.attribute("maketx:filtername", gFilterNames[filter_]);

        if (fixNan_) {
            if (nanFixType_)
                destSpec.attribute("maketx:fixnan", "box3");
            else
                destSpec.attribute("maketx:fixnan", "black");
        }
        else
            destSpec.attribute("maketx:fixnan", "none");

        destSpec.attribute("maketx:checknan", checkNan_);
        destSpec.attribute("maketx:verbose", verbose_);
        destSpec.attribute("maketx:stats", stats_);
        destSpec.attribute("maketx:oiio_options", true);

        OIIO::attribute("threads", (int)Thread::numCPUs);

        if (aborted())
            return;

        iop->progressMessage("Writing %s", filename());
        if (!ImageBufAlgo::make_texture(ImageBufAlgo::MakeTxTexture, srcBuffer,
                                        filename(), destSpec, &std::cout))
            iop->critical("ImageBufAlgo::make_texture failed to write file %s",
                          filename());
    }

    const char* help() { return "Tiled, mipmapped texture format"; }

    static const Writer::Description d;
};


}  // ~TxWriterNS


static Writer* build(Write* iop) { return new TxWriterNS::txWriter(iop); }
const Writer::Description TxWriterNS::txWriter::d("tx\0TX\0", build);