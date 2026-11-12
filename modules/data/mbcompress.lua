sptbl["mbcompress"] = {

    files = {
        module = "mbcompress.c",
        header = "mbcompress.h",
        example = "ex_mbcompress.c",
    },

    func = {
        create = "sp_mbcompress_create",
        destroy = "sp_mbcompress_destroy",
        init = "sp_mbcompress_init",
        compute = "sp_mbcompress_compute",
    },

    params = {
        mandatory = {
            {
                name = "f1",
                type = "SPFLOAT",
                description = "Low/mid crossover frequency in Hz, fixed at init (0 < f1 < f2 < sr/2)",
                default = 200
            },
            {
                name = "f2",
                type = "SPFLOAT",
                description = "Mid/high crossover frequency in Hz, fixed at init (f1 < f2 < sr/2)",
                default = 2000
            },
        },

        optional = {
            {
                name = "thresholdDb",
                type = "SPFLOAT",
                description = "Per-band compression threshold in dB, set via sp_mbcompress_threshold",
                default = -12
            },
            {
                name = "ratio",
                type = "SPFLOAT",
                description = "Per-band compression ratio (>= 1), set via sp_mbcompress_ratio",
                default = 1
            },
            {
                name = "attackSeconds",
                type = "SPFLOAT",
                description = "Per-band attack time in seconds (>= 0, 0 is instant), set via sp_mbcompress_attack",
                default = 0.01
            },
            {
                name = "releaseSeconds",
                type = "SPFLOAT",
                description = "Per-band release time in seconds (>= 0, 0 is instant), set via sp_mbcompress_release",
                default = 0.1
            },
            {
                name = "wet",
                type = "SPFLOAT",
                description = "Linear dry/wet mix in [0, 1]; the dry path shares the crossover all-pass phase",
                default = 1
            },
        }
    },

    modtype = "module",

    description = [[Stereo three-band compressor with fourth-order
Linkwitz-Riley crossovers. The three bands recombine to the unit-gain
all-pass transfer function A_f1(z) * A_f2(z), and the dry path is
filtered through that same all-pass so dry/wet mixes never comb-filter.
Each band has an independent stereo-linked compressor.]],

    ninputs = 2,
    noutputs = 2,

    inputs = {
        {
            name = "inL",
            description = "Left channel input."
        },
        {
            name = "inR",
            description = "Right channel input."
        },
    },

    outputs = {
        {
            name = "outL",
            description = "Left channel output."
        },
        {
            name = "outR",
            description = "Right channel output."
        },
    }

}
