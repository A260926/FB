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
                description = [[First crossover frequency in Hz,
fixed at init time. Requires 0 < f1 < f2 < sr/2.]],
                default = 200
            },
            {
                name = "f2",
                type = "SPFLOAT",
                description = [[Second crossover frequency in Hz,
fixed at init time. Requires 0 < f1 < f2 < sr/2.]],
                default = 2000
            },
        },

        optional = {
            {
                name = "wet",
                type = "SPFLOAT",
                description = [[Dry/wet balance from 0 (fully
phase-aligned dry) to 1 (fully compressed).]],
                default = 1
            },
        }
    },

    modtype = "module",

    description = [[Stereo three-band compressor with 4th-order
Linkwitz-Riley crossovers. Each band has an independent threshold
(decibels), ratio, attack and release (seconds), set through the
sp_mbcompress_set_* functions. Band detectors use the larger absolute
value of the left and right signals, so one gain is shared by both
channels of a band. The dry path follows the same all-pass response
as the summed bands, preventing comb filtering at the crossovers.]],

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
