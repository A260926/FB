#ifndef SIGNALSMITH_STRETCH_H
#define SIGNALSMITH_STRETCH_H

#include "signalsmith-linear/stft.h" // https://github.com/Signalsmith-Audio/linear

#include <vector>
#include <array>
#include <algorithm>
#include <functional>
#include <random>
#include <limits>
#include <type_traits>

namespace signalsmith { namespace stretch {

namespace _impl {
	template<bool conjugateSecond=false, typename V>
	static std::complex<V> mul(const std::complex<V> &a, const std::complex<V> &b) {
		return conjugateSecond ? std::complex<V>{
			b.real()*a.real() + b.imag()*a.imag(),
				b.real()*a.imag() - b.imag()*a.real()
		} : std::complex<V>{
			a.real()*b.real() - a.imag()*b.imag(),
			a.real()*b.imag() + a.imag()*b.real()
		};
	}
	template<typename V>
	static V norm(const std::complex<V> &a) {
		V r = a.real(), i = a.imag();
		return r*r + i*i;
	}
}

template<typename Sample=float, class RandomEngine=void>
struct SignalsmithStretch {
	static constexpr size_t version[3] = {1, 3, 2};

	SignalsmithStretch() : randomEngine(std::random_device{}()) {}
	SignalsmithStretch(long seed) : randomEngine(seed) {}
		
	// The difference between the internal position (centre of a block) and the input samples you're supplying
	int inputLatency() const {
		return int(stft.analysisLatency());
	}
	int outputLatency() const {
		return int(stft.synthesisLatency() + _splitComputation*stft.defaultInterval());
	}
	
	void reset() {
		stft.reset(0.1);
		stashedInput = stft.input;
		stashedOutput = stft.output;

		prevInputOffset = -1;
		_channelBands.assign(_channelBands.size(), Band());
		silenceCounter = 0;
		didSeek = false;
		blockProcess = {};
		freqEstimateWeighted = freqEstimateWeight = 0;
		clearSpectralFreeze();
	}

	// Configures using a default preset
	void presetDefault(int nChannels, Sample sampleRate, bool splitComputation=false) {
		configure(nChannels, static_cast<int>(sampleRate*0.12), static_cast<int>(sampleRate*0.03), splitComputation);
	}
	void presetCheaper(int nChannels, Sample sampleRate, bool splitComputation=true) {
		configure(nChannels, static_cast<int>(sampleRate*0.1), static_cast<int>(sampleRate*0.04), splitComputation);
	}

	// Manual setup
	void configure(int nChannels, int blockSamples, int intervalSamples, bool splitComputation=false) {
		_splitComputation = splitComputation;
		channels = nChannels;
		stft.configure(channels, channels, blockSamples, intervalSamples + 1);
		stft.setInterval(intervalSamples, stft.kaiser);
		stft.reset(Sample(0.1));
		stashedInput = stft.input;
		stashedOutput = stft.output;

		bands = int(stft.bands());
		_channelBands.assign(bands*channels, Band());
		
		peaks.reserve(bands/2);
		energy.resize(bands);
		smoothedEnergy.resize(bands);
		outputMap.resize(bands);
		channelPredictions.resize(channels*bands);

		blockProcess = {};
		formantMetric.resize(bands + 2);

		tmpProcessBuffer.resize(blockSamples + intervalSamples);
		tmpPreRollBuffer.resize(outputLatency()*channels);

		clearSpectralFreeze();
		// Freeze analysis history: two most recent spectra (each `bands` complex values per channel)
		freezeHistory.assign(size_t(2)*size_t(channels)*size_t(bands), Complex(0));
		freezeRegionEnergy.resize(bands);
		freezeMag.assign(size_t(channels)*size_t(bands), 0);
		freezePhase.assign(size_t(channels)*size_t(bands), Complex(0));
		freezeFreqRot.assign(bands, Complex(0));
		freezeBinFreq.assign(bands, 0);
		freezeLockedTo.assign(bands, 0);
		freezePeaks.reserve(bands/2);
		freezeBoundaries.reserve(bands/2);
	}
	// For querying the existing config
	int blockSamples() const {
		return int(stft.blockSamples());
	}
	int intervalSamples() const {
		return int(stft.defaultInterval());
	}
	bool splitComputation() const {
		return _splitComputation;
	}

	/// Frequency multiplier, and optional tonality limit (as multiple of sample-rate)
	void setTransposeFactor(Sample multiplier, Sample tonalityLimit=0) {
		freqMultiplier = multiplier;
		if (tonalityLimit > 0) {
			freqTonalityLimit = tonalityLimit/std::sqrt(multiplier); // compromise between input and output limits
		} else {
			freqTonalityLimit = 1;
		}
		customFreqMap = nullptr;
	}
	void setTransposeSemitones(Sample semitones, Sample tonalityLimit=0) {
		setTransposeFactor(std::pow(2, semitones/12), tonalityLimit);
	}
	// Sets a custom frequency map - should be monotonically increasing
	void setFreqMap(std::function<Sample(Sample)> inputToOutput) {
		customFreqMap = inputToOutput;
	}

	void setFormantFactor(Sample multiplier, bool compensatePitch=false) {
		formantMultiplier = multiplier;
		invFormantMultiplier = 1/multiplier;
		formantCompensation = compensatePitch;
	}
	void setFormantSemitones(Sample semitones, bool compensatePitch=false) {
		setFormantFactor(std::pow(2, semitones/12), compensatePitch);
	}
	// Rough guesstimate of the fundamental frequency, used for formant analysis. 0 means attempting to detect the pitch
	void setFormantBase(Sample baseFreq=0) {
		formantBaseFreq = baseFreq;
	}

	/// Spectral freeze ("hold the current timbre"): while enabled, the output continues from the last analysed
	/// spectrum, using its per-bin energy and instantaneous frequency.  The real input keeps being analysed.
	/// The request is latched at the start of the next synthesis hop.  `false` fades back to the live spectrum
	/// over four complete hops.
	void setSpectralFreeze(bool enabled) {
		freezeRequested = enabled;
	}
	bool spectralFreezeActive() const {
		return freezeLatched && freezeSnapshotValid;
	}
	
	// Provide previous input ("pre-roll") to smoothly change the input location without interrupting the output.  This doesn't do any calculation, just copies intput to a buffer.
	// You should ideally feed it `seekLength()` frames of input, unless it's directly after a `.reset()` (in which case `.outputSeek()` might be a better choice)
	template<class Inputs>
	void seek(Inputs &&inputs, int inputSamples, double playbackRate) {
		tmpProcessBuffer.resize(0);
		tmpProcessBuffer.resize(stft.blockSamples() + stft.defaultInterval());

		int startIndex = std::max<int>(0, inputSamples - int(tmpProcessBuffer.size())); // start position in input
		int padStart = int(tmpProcessBuffer.size() + startIndex) - inputSamples; // start position in tmpProcessBuffer

		Sample totalEnergy = 0;
		for (int c = 0; c < channels; ++c) {
			auto &&inputChannel = inputs[c];
			for (int i = startIndex; i < inputSamples; ++i) {
				Sample s = inputChannel[i];
				totalEnergy += s*s;
				tmpProcessBuffer[i - startIndex + padStart] = s;
			}
			
			stft.writeInput(c, tmpProcessBuffer.size(), tmpProcessBuffer.data());
		}
		stft.moveInput(tmpProcessBuffer.size());
		if (totalEnergy >= noiseFloor) {
			silenceCounter = 0;
			silenceFirst = true;
		}
		didSeek = true;
		seekTimeFactor = (playbackRate*stft.defaultInterval() > 1) ? 1/playbackRate : stft.defaultInterval();
		clearSpectralFreeze(); // a seek starts a new stream: drop any freeze request/snapshot
	}
	int seekLength() const {
		return int(stft.blockSamples() + stft.defaultInterval());
	}
	
	// Moves the input position *and* pre-calculates some output, so that the next samples returned from `.process()` are aligned to the beginning of the sample.
	// The time-stretch rate is inferred from `inputLength`, so use `.outputSeekLength()` to get a correct value for that.
	template<class Inputs>
	void outputSeek(Inputs &&inputs, int inputLength) {
		// TODO: add fade-out parameter to avoid clicks, instead of doing a full reset
		reset();
		// Assume we've been handed enough surplus input to produce `outputLatency()` samples of pre-roll
		int surplusInput = std::max<int>(inputLength - inputLatency(), 0);
		Sample playbackRate = surplusInput/Sample(outputLatency());

		// Move the input position to the start of the sound
		int seekSamples = inputLength - surplusInput;
		seek(inputs, seekSamples, playbackRate);
		
		tmpPreRollBuffer.resize(outputLatency()*channels);
		struct BufferOutput {
			Sample *samples;
			int length;
			
			Sample * operator[](int c) {
				return samples + c*length;
			}
		} preRollOutput{tmpPreRollBuffer.data(), outputLatency()};
		
		// Use the surplus input to produce pre-roll output
		OffsetIO<Inputs> offsetInput{inputs, seekSamples};
		process(offsetInput, surplusInput, preRollOutput, preRollOutput.length);
		
		// put the thing down, flip it and reverse it
		for (auto &v : tmpPreRollBuffer) v = -v;
		for (int c = 0; c < channels; ++c) {
			std::reverse(preRollOutput[c], preRollOutput[c] + preRollOutput.length);
			stft.addOutput(c, preRollOutput.length, preRollOutput[c]);
		}
	}
	int outputSeekLength(Sample playbackRate) const {
		return inputLatency() + playbackRate*outputLatency();
	}

	template<class Inputs, class Outputs>
	void process(Inputs &&inputs, int inputSamples, Outputs &&outputs, int outputSamples) {
#ifdef SIGNALSMITH_STRETCH_PROFILE_PROCESS_START
		SIGNALSMITH_STRETCH_PROFILE_PROCESS_START(inputSamples, outputSamples);
#endif
		int prevCopiedInput = 0;
		auto copyInput = [&](int toIndex){

			int length = std::min<int>(int(stft.blockSamples() + stft.defaultInterval()), toIndex - prevCopiedInput);
			tmpProcessBuffer.resize(length);
			int offset = toIndex - length;
			for (int c = 0; c < channels; ++c) {
				auto &&inputBuffer = inputs[c];
				for (int i = 0; i < length; ++i) {
					tmpProcessBuffer[i] = inputBuffer[i + offset];
				}
				stft.writeInput(c, length, tmpProcessBuffer.data());
			}
			stft.moveInput(length);
			prevCopiedInput = toIndex;
		};

		Sample totalEnergy = 0;
		for (int c = 0; c < channels; ++c) {
			auto &&inputChannel = inputs[c];
			for (int i = 0; i < inputSamples; ++i) {
				Sample s = inputChannel[i];
				totalEnergy += s*s;
			}
		}

		if (totalEnergy < noiseFloor) {
			if (silenceCounter >= 2*stft.blockSamples() && !freezeApplyingSpectrum()) {
				if (silenceFirst) { // first block of silence processing
					silenceFirst = false;
					//stft.reset();
					blockProcess = {};
					for (auto &b : _channelBands) {
						b.input = b.prevInput = b.output = 0;
						b.inputEnergy = 0;
					}
					// No real analysis is running here, so the spectra in the capture history are
					// stale.  Drop them: a freeze requested now must wait for two fresh analysis
					// frames instead of resurrecting the timbre from before the silence.
					freezeHistoryCount = 0;
					freezeHistoryValidRun = 0;
				}

				if (inputSamples > 0) {
					// copy from the input, wrapping around if needed
					for (int outputIndex = 0; outputIndex < outputSamples; ++outputIndex) {
						int inputIndex = outputIndex%inputSamples;
						for (int c = 0; c < channels; ++c) {
							outputs[c][outputIndex] = inputs[c][inputIndex];
						}
					}
				} else {
					for (int c = 0; c < channels; ++c) {
						auto &&outputChannel = outputs[c];
						for (int outputIndex = 0; outputIndex < outputSamples; ++outputIndex) {
							outputChannel[outputIndex] = 0;
						}
					}
				}

				// Store input in history buffer
				copyInput(inputSamples);
				return;
			} else {
				silenceCounter += inputSamples;
			}
		} else {
			silenceCounter = 0;
			silenceFirst = true;
		}
		
		for (int outputIndex = 0; outputIndex < outputSamples; ++outputIndex) {
			bool newBlock = blockProcess.samplesSinceLast >= stft.defaultInterval();
			if (newBlock) {
				blockProcess.step = 0;
				blockProcess.steps = 0; // how many processing steps this block will have
				blockProcess.samplesSinceLast = 0;

				// Latch any pending freeze request at the very start of this hop, then advance the
				// release ramp.  Hops which are already running keep the state they started with.
				latchFreezeRequest();
				beginFreezeHop();

				// Time to process a spectrum!  Where should it come from in the input?
				int inputOffset = static_cast<int>(std::round(outputIndex*Sample(inputSamples)/outputSamples));
				int inputInterval = inputOffset - prevInputOffset;
				prevInputOffset = inputOffset;

				copyInput(inputOffset);
				stashedInput = stft.input; // save the input state, since that's what we'll analyse later
				if (_splitComputation) {
					stashedOutput = stft.output; // save the current output, and read from it
					stft.moveOutput(stft.defaultInterval()); // the actual input jumps forward in time by one interval, ready for the synthesis
				}

				blockProcess.newSpectrum = didSeek || (inputInterval > 0);
				blockProcess.mappedFrequencies = customFreqMap || freqMultiplier != 1;
				blockProcess.analysisInterval = std::max(1, didSeek ? int(stft.defaultInterval()) : inputInterval);
				if (blockProcess.newSpectrum) {
					// make sure the previous input is the correct distance in the past (give or take 1 sample)
					blockProcess.reanalysePrev = didSeek || std::abs(inputInterval - int(stft.defaultInterval())) > 1;
					if (blockProcess.reanalysePrev) blockProcess.steps += stft.analyseSteps() + 1;

					// analyse a new input
					blockProcess.steps += stft.analyseSteps() + 1;
				}

				blockProcess.processFormants = formantMultiplier != 1 || (formantCompensation && blockProcess.mappedFrequencies);

				blockProcess.timeFactor = didSeek ? seekTimeFactor : stft.defaultInterval()/static_cast<Sample>(std::max(1, inputInterval));
				didSeek = false;

				updateProcessSpectrumSteps();
				blockProcess.steps += processSpectrumSteps;

				blockProcess.steps += stft.synthesiseSteps() + 1;
			}
			
			size_t processToStep = newBlock ? blockProcess.steps : 0;
			if (_splitComputation) {
				Sample processRatio = Sample(blockProcess.samplesSinceLast + 1)/stft.defaultInterval();
				processToStep = std::min(blockProcess.steps, static_cast<size_t>((blockProcess.steps + 0.999f)*processRatio));
			}
			
			while (blockProcess.step < processToStep) {
				size_t step = blockProcess.step++;
#ifdef SIGNALSMITH_STRETCH_PROFILE_PROCESS_STEP
				SIGNALSMITH_STRETCH_PROFILE_PROCESS_STEP(step, blockProcess.steps);
#endif
				if (blockProcess.newSpectrum) {
					if (blockProcess.reanalysePrev) {
						// analyse past input
						if (step < stft.analyseSteps()) {
							stashedInput.swap(stft.input);
							stft.analyseStep(step, stft.defaultInterval());
							stashedInput.swap(stft.input);
							continue;
						}
						step -= stft.analyseSteps();
						if (step < 1) {
							// Copy previous analysis to our band objects
							for (int c = 0; c < channels; ++c) {
								auto channelBands = bandsForChannel(c);
								auto *spectrumBands = stft.spectrum(c);
								for (int b = 0; b < bands; ++b) {
									channelBands[b].prevInput = spectrumBands[b];
								}
							}
							continue;
						}
						step -= 1;
					}

					// Analyse latest (stashed) input
					if (step < stft.analyseSteps()) {
						stashedInput.swap(stft.input);
						stft.analyseStep(step);
						stashedInput.swap(stft.input);
						continue;
					}
					step -= stft.analyseSteps();
					if (step < 1) {
						// Copy analysed spectrum into our band objects
						for (int c = 0; c < channels; ++c) {
							auto channelBands = bandsForChannel(c);
							auto *spectrumBands = stft.spectrum(c);
							for (int b = 0; b < bands; ++b) {
								channelBands[b].input = spectrumBands[b];
							}
						}
						// A real spectrum has arrived: keep the two most recent spectra so a
						// freeze can capture from them the moment it is requested.  (The
						// silence short-cut above discards them if the input goes quiet.)
						pushFreezeHistory(Sample(std::max(1, blockProcess.analysisInterval)));
						if (freezeLatched && !freezeSnapshotValid && freezeHistoryReady()) {
							captureFreezeSnapshot();
						}
						continue;
					}
					step -= 1;
				}

				if (step < processSpectrumSteps) {
					processSpectrum(step);
					continue;
				}
				step -= processSpectrumSteps;

				if (step < 1) {
					// Copy band objects into spectrum
					for (int c = 0; c < channels; ++c) {
						auto channelBands = bandsForChannel(c);
						auto *spectrumBands = stft.spectrum(c);
						for (int b = 0; b < bands; ++b) {
							spectrumBands[b] = channelBands[b].output;
						}
					}
					// Spectral freeze: substitute the frozen synthesis spectrum (blended during
					// release), then advance the persistent oscillators to the next hop.
					if (freezeApplyingSpectrum()) {
						applyFrozenSpectrum(freezeBlendGain);
						for (int c = 0; c < channels; ++c) {
							for (int b = 0; b < bands; ++b) {
								size_t index = size_t(c)*size_t(bands) + size_t(b);
								freezePhase[index] = _impl::mul(freezePhase[index], freezeFreqRot[b]);
							}
						}
					}
					continue;
				}
				step -= 1;
				
				if (step < stft.synthesiseSteps()) {
					stft.synthesiseStep(step);
					continue;
				}
			}
#ifdef SIGNALSMITH_STRETCH_PROFILE_PROCESS_ENDSTEP
			SIGNALSMITH_STRETCH_PROFILE_PROCESS_ENDSTEP();
#endif

			++blockProcess.samplesSinceLast;
			if (_splitComputation) stashedOutput.swap(stft.output);
			for (int c = 0; c < channels; ++c) {
				auto &&outputChannel = outputs[c];
				Sample v = 0;
				stft.readOutput(c, 1, &v);
				outputChannel[outputIndex] = v;
			}
			stft.moveOutput(1);
			if (_splitComputation) stashedOutput.swap(stft.output);
		}
		
		copyInput(inputSamples);
		prevInputOffset -= inputSamples;
#ifdef SIGNALSMITH_STRETCH_PROFILE_PROCESS_END
		SIGNALSMITH_STRETCH_PROFILE_PROCESS_END();
#endif
	}

	// Read the remaining output, providing no further input.  If `outputSamples` is more than one interval, it will compute additional blocks assuming a zero-valued input
	template<class Outputs>
	void flush(Outputs &&outputs, int outputSamples, Sample playbackRate=0) {
		// Any freeze is dropped before flushing: the tail is the normal finite impulse response
		// of whatever was already in the synthesis buffer, never an indefinite held tone.  Reset
		// the silence counter so the pending synthesis tail is rendered smoothly rather than
		// being short-circuited into a discontinuity.
		bool wasFrozen = freezeHolding();
		clearSpectralFreeze();
		if (wasFrozen) {
			silenceCounter = 0;
			silenceFirst = true;
		}
		struct Zeros {
			struct Channel {
				Sample operator[](int) {
					return 0;
				}
			};
			Channel operator[](int) {
				return {};
			}
		} zeros;
		// If we're asked for more than an interval of extra output, then zero-pad the input
		int outputBlock = std::max<int>(0, outputSamples - stft.defaultInterval());
		if (outputBlock > 0) process(zeros, outputBlock*playbackRate, outputs, outputBlock);

		int tailSamples = outputSamples - outputBlock; // at most one interval
		tmpProcessBuffer.resize(tailSamples);
		stft.finishOutput(1);
		for (int c = 0; c < channels; ++c) {
			stft.readOutput(c, tailSamples, tmpProcessBuffer.data());
			auto &&outputChannel = outputs[c];
			for (int i = 0; i < tailSamples; ++i) {
				outputChannel[outputBlock + i] = tmpProcessBuffer[i];
			}
			stft.readOutput(c, tailSamples, tailSamples, tmpProcessBuffer.data());
			for (int i = 0; i < tailSamples; ++i) {
				outputChannel[outputBlock + tailSamples - 1 - i] -= tmpProcessBuffer[i];
			}
		}
		stft.reset(0.1f);
		// Reset the phase-vocoder stuff, so the next block gets a fresh start
		for (int c = 0; c < channels; ++c) {
			auto channelBands = bandsForChannel(c);
			for (int b = 0; b < bands; ++b) {
				channelBands[b].prevInput = channelBands[b].output = 0;
			}
		}
	}

	// Process a complete audio buffer all in one go
	template<class Inputs, class Outputs>
	bool exact(Inputs &&inputs, int inputSamples, Outputs &&outputs, int outputSamples) {
		Sample playbackRate = inputSamples/Sample(outputSamples);
		auto seekLength = outputSeekLength(playbackRate);
		if (inputSamples < seekLength) {
			// to short for this - zero the output just to be polite
			for (int c = 0; c < channels; ++c) {
				auto &&channel = outputs[c];
				for (int i = 0; i < outputSamples; ++i) {
					channel[i] = 0;
				}
			}
			return false;
		}

		outputSeek(inputs, seekLength);

		int outputIndex = outputSamples - seekLength/playbackRate;
		OffsetIO<Inputs> offsetInput{inputs, seekLength};
		process(offsetInput, inputSamples - seekLength, outputs, outputIndex);
		
		OffsetIO<Outputs> offsetOutput{outputs, outputIndex};
		flush(offsetOutput, outputSamples - outputIndex, playbackRate);
		return true;
	}

private:
	bool _splitComputation = false;
	struct {
		size_t samplesSinceLast = std::numeric_limits<size_t>::max();
		size_t steps = 0;
		size_t step = 0;
		
		bool newSpectrum = false;
		bool reanalysePrev = false;
		bool mappedFrequencies = false;
		bool processFormants = false;
		int analysisInterval = 1; // samples advanced since the previous real analysis
		Sample timeFactor;
	} blockProcess;

	using Complex = std::complex<Sample>;
	static constexpr Sample noiseFloor{Sample(1e-15)};
	static constexpr Sample maxCleanStretch{2}; // time-stretch ratio before we start randomising phases
	size_t silenceCounter = 0;
	bool silenceFirst = true;

	// --- Spectral freeze -------------------------------------------------
	// The two most recent analysis spectra are kept in a tiny rolling history.  On capture, each
	// bin's amplitude and instantaneous frequency are taken from them; each channel keeps its own
	// amplitude/phase, so the inter-channel amplitude ratio and phase difference are preserved.
	// During the freeze the real input is still analysed normally -- only the synthesis spectrum is
	// replaced, so latency and the live pipeline state stay intact for a clean release.
	static constexpr int freezeReleaseHops = 4;
	bool freezeRequested = false; // what the caller last asked for
	bool freezeLatched = false; // the request, as latched at the start of the current hop
	int freezeHistoryCount = 0; // valid spectra in `freezeHistory` (0..2)
	int freezeHistoryValidRun = 0; // consecutive non-silent spectra pushed (0..2)
	int freezeReleaseHopsDone = 0;
	int freezeReleaseHopsTotal = 0;
	bool freezeSnapshotValid = false;
	Sample freezeAnalysisDelta = 1; // samples between the two history spectra
	std::vector<Complex> freezeHistory; // [slot][channel][band]
	Complex * freezeHistorySlot(int slot) {
		return freezeHistory.data() + size_t(slot)*size_t(channels)*size_t(bands);
	}
	std::vector<Sample> freezeMag; // captured per-channel amplitude, [channel][band]
	std::vector<Complex> freezePhase; // per-channel advancing synthesis phasor, [channel][band]
	std::vector<Complex> freezeFreqRot; // per-band phase advance for one hop
	std::vector<Sample> freezeBinFreq; // per-band captured instantaneous frequency (normalised)
	std::vector<int> freezeLockedTo; // per-band phase-locked peak bin
	std::vector<Sample> freezeRegionEnergy; // scratch: summed energy per bin
	std::vector<int> freezePeaks; // scratch: peak bins found at capture
	std::vector<int> freezeBoundaries; // scratch: boundaries between peak regions
	Sample freezeBlendGain = 0; // 0 = frozen, 1 = fully live (during release)

	bool freezeHolding() const {
		return freezeLatched || freezeReleaseHopsDone < freezeReleaseHopsTotal;
	}
	bool freezeApplyingSpectrum() const {
		return freezeSnapshotValid && freezeHolding();
	}

	void clearSpectralFreeze() {
		freezeRequested = false;
		freezeLatched = false;
		freezeReleaseHopsDone = 0;
		freezeReleaseHopsTotal = 0;
		freezeSnapshotValid = false;
		freezeHistoryCount = 0;
		freezeHistoryValidRun = 0;
		freezeAnalysisDelta = 1;
		freezeBlendGain = 0;
		std::fill(freezePhase.begin(), freezePhase.end(), Complex(0));
	}

	// Called at the start of each synthesis hop, before any spectrum work for that hop.
	void latchFreezeRequest() {
		if (freezeRequested == freezeLatched) return; // already latched: never restart a held freeze
		freezeLatched = freezeRequested;
		if (freezeLatched) {
			// Entering freeze: capture from the most recent real analysis, if we have two.
			// (When re-pressed during a release, this is the current live analysis.)
			freezeSnapshotValid = false;
			freezeReleaseHopsDone = freezeReleaseHopsTotal = 0;
			freezeBlendGain = 0;
			if (freezeHistoryReady()) captureFreezeSnapshot();
		} else {
			// Release: fade from the frozen spectrum back to the live analysis over four whole hops.
			freezeReleaseHopsDone = 0;
			freezeReleaseHopsTotal = freezeReleaseHops;
		}
	}

	// Advance the freeze/release state at the start of a hop (after latching).
	void beginFreezeHop() {
		if (freezeLatched) {
			freezeBlendGain = 0;
			if (!freezeSnapshotValid && freezeHistoryReady()) captureFreezeSnapshot();
		} else if (freezeReleaseHopsDone < freezeReleaseHopsTotal) {
			// Four complete hops to fade from the frozen spectrum to the live analysis:
			// gains 1/4, 2/4, 3/4, 4/4.  After the fourth hop, the output is purely live.
			++freezeReleaseHopsDone;
			freezeBlendGain = Sample(freezeReleaseHopsDone)/Sample(freezeReleaseHopsTotal);
			if (freezeReleaseHopsDone >= freezeReleaseHopsTotal) freezeSnapshotValid = false;
		} else {
			freezeBlendGain = 1;
		}
	}

	// Push a newly-analysed live spectrum into the two-slot history.  Spectra which carry no
	// energy are not "valid" capture material (only silence so far), so they reset the validity
	// run: a freeze waits until two real analysis spectra have been seen.
	void pushFreezeHistory(Sample analysisDelta) {
		if (freezeHistory.size() < size_t(2)*size_t(channels)*size_t(bands)) return;
		if (freezeHistoryCount >= 2) {
			Complex *a = freezeHistorySlot(0), *b = freezeHistorySlot(1);
			std::copy(b, b + size_t(channels)*size_t(bands), a);
			freezeHistoryCount = 1;
		}
		Complex *dest = freezeHistorySlot(freezeHistoryCount);
		Sample energy = 0;
		for (int c = 0; c < channels; ++c) {
			auto *bins = bandsForChannel(c);
			for (int b = 0; b < bands; ++b) {
				Complex z = bins[b].input;
				dest[size_t(c)*size_t(bands) + size_t(b)] = z;
				energy += _impl::norm(z);
			}
		}
		++freezeHistoryCount;
		if (energy > noiseFloor) {
			if (freezeHistoryValidRun < 2) ++freezeHistoryValidRun;
		} else {
			freezeHistoryValidRun = 0;
		}
		// Once we have two consecutive real spectra, the distance between them is the analysis
		// hop used for instantaneous-frequency estimation.
		if (freezeHistoryValidRun >= 2 && freezeHistoryCount >= 2) {
			freezeAnalysisDelta = std::max<Sample>(1, analysisDelta);
		}
	}
	bool freezeHistoryReady() const {
		return freezeHistoryValidRun >= 2 && freezeHistoryCount >= 2;
	}

	// Build the fixed snapshot from the two most recent analysis spectra.
	void captureFreezeSnapshot() {
		if (freezeHistoryCount < 2) return;
		size_t count = size_t(bands);
		if (freezeMag.size() != size_t(channels)*count) freezeMag.resize(size_t(channels)*count);
		if (freezePhase.size() != size_t(channels)*count) freezePhase.assign(size_t(channels)*count, Complex(0));
		if (freezeFreqRot.size() != count) {
			freezeFreqRot.resize(count);
			freezeBinFreq.resize(count);
			freezeLockedTo.resize(count);
		}

		Complex *prev = freezeHistorySlot(0);
		Complex *cur = freezeHistorySlot(1);
		Sample delta = std::max<Sample>(1, freezeAnalysisDelta);
		// Phase advance of each bin, relative to its bin centre, wrapped into ±half a cycle over
		// the analysis distance.  This gives the instantaneous frequency without snapping to the
		// FFT grid.  Phases are summed across channels so the estimate is robust for anti-phase
		// material too.
		for (int b = 0; b < bands; ++b) {
			Complex twist = 0;
			for (int c = 0; c < channels; ++c) {
				size_t index = size_t(c)*count + size_t(b);
				twist += _impl::mul<true>(cur[index], prev[index]);
			}
			Sample measured = std::arg(twist)/(Sample(2*M_PI)); // cycles per `delta` samples
			Sample expected = stft.binToFreq(Sample(b))*delta;
			Sample offset = measured - expected;
			offset -= std::floor(offset + Sample(0.5));
			Sample freq = stft.binToFreq(Sample(b)) + offset/delta;
			freezeBinFreq[b] = std::min<Sample>(std::max<Sample>(freq, 0), Sample(0.5));
		}

		// Identity phase-locking: within a spectral mainlobe, all bins share the peak bin's
		// frequency (their relative phases stay fixed).  Without this, independent per-bin
		// advance drifts apart and the frozen tone decays over time.
		{
			// Total energy per bin, across channels.
			if (freezeRegionEnergy.size() != count) freezeRegionEnergy.resize(count);
			for (int b = 0; b < bands; ++b) {
				Sample e = 0;
				for (int c = 0; c < channels; ++c) e += _impl::norm(cur[size_t(c)*count + size_t(b)]);
				freezeRegionEnergy[b] = e;
			}
			// Local maxima (non-strict, so flat noise still produces sensible regions).
			freezePeaks.clear();
			for (int b = 0; b < bands; ++b) {
				Sample e = freezeRegionEnergy[b];
				bool left = (b == 0) || e >= freezeRegionEnergy[b - 1];
				bool right = (b == bands - 1) || e > freezeRegionEnergy[b + 1];
				if (left && right && e > noiseFloor) freezePeaks.push_back(b);
			}
			const std::vector<int> &peakBins = freezePeaks;
			if (peakBins.size() > 0) {
				// Boundary between adjacent peaks = the lowest-energy bin between them.
				freezeBoundaries.clear();
				freezeBoundaries.reserve(peakBins.size());
				for (size_t p = 1; p < peakBins.size(); ++p) {
					int lo = peakBins[p - 1], hi = peakBins[p];
					int split = lo;
					Sample minE = freezeRegionEnergy[lo];
					for (int i = lo; i < hi; ++i) {
						if (freezeRegionEnergy[i] < minE) {
							minE = freezeRegionEnergy[i];
							split = i;
						}
					}
					freezeBoundaries.push_back(split);
				}
				const std::vector<int> &boundaries = freezeBoundaries;
				size_t region = 0;
				for (int b = 0; b < bands; ++b) {
					while (region < boundaries.size() && b > boundaries[region]) ++region;
					freezeLockedTo[b] = peakBins[region];
				}
				for (int b = 0; b < bands; ++b) {
					freezeBinFreq[b] = freezeBinFreq[freezeLockedTo[b]];
				}
			}
		}
		for (int b = 0; b < bands; ++b) {
			freezeFreqRot[b] = std::polar(Sample(1), Sample(2*M_PI)*freezeBinFreq[b]*Sample(stft.defaultInterval()));
			for (int c = 0; c < channels; ++c) {
				Complex z = cur[size_t(c)*count + size_t(b)];
				Sample mag = std::abs(z);
				freezeMag[size_t(c)*count + size_t(b)] = mag;
				// Keep the captured absolute phase; both channels advance identically, so their
				// relative phase difference is preserved.
				freezePhase[size_t(c)*count + size_t(b)] = (mag > noiseFloor) ? z/mag : Complex(0);
			}
		}
		freezeSnapshotValid = true;
	}

	// Replace the synthesis spectrum with the frozen one, blended towards the live spectrum.
	// Called once per hop, after the live processSpectrum has run.  This writes to the STFT
	// spectrum only, so the live phase-vocoder band state stays untouched for a clean release.
	void applyFrozenSpectrum(Sample blend) {
		for (int c = 0; c < channels; ++c) {
			auto *spectrumBands = stft.spectrum(c);
			for (int b = 0; b < bands; ++b) {
				size_t index = size_t(c)*size_t(bands) + size_t(b);
				Complex frozen = freezePhase[index]*freezeMag[index];
				spectrumBands[b] = frozen*(Sample(1) - blend) + spectrumBands[b]*blend;
			}
		}
	}

	Sample freqMultiplier = 1, freqTonalityLimit = 0.5;
	std::function<Sample(Sample)> customFreqMap = nullptr;
	
	bool formantCompensation = false; // compensate for pitch/freq change
	Sample formantMultiplier = 1, invFormantMultiplier = 1;

	using STFT = signalsmith::linear::DynamicSTFT<Sample, false, true>;
	STFT stft;
	typename STFT::Input stashedInput;
	typename STFT::Output stashedOutput;
	
	std::vector<Sample> tmpProcessBuffer, tmpPreRollBuffer;

	int channels = 0, bands = 0;
	int prevInputOffset = -1;
	bool didSeek = false;
	Sample seekTimeFactor = 1;

	Sample bandToFreq(Sample b) const {
		return stft.binToFreq(b);
	}
	Sample freqToBand(Sample f) const {
		return stft.freqToBin(f);
	}
	
	struct Band {
		Complex input, prevInput{0};
		Complex output{0};
		Sample inputEnergy;
	};
	std::vector<Band> _channelBands;
	Band * bandsForChannel(int channel) {
		return _channelBands.data() + channel*bands;
	}
	template<Complex Band::*member>
	Complex getBand(int channel, int index) {
		if (index < 0 || index >= bands) return 0;
		return _channelBands[index + channel*bands].*member;
	}
	template<Complex Band::*member>
	Complex getFractional(int channel, int lowIndex, Sample fractional) {
		Complex low = getBand<member>(channel, lowIndex);
		Complex high = getBand<member>(channel, lowIndex + 1);
		return low + (high - low)*fractional;
	}
	template<Complex Band::*member>
	Complex getFractional(int channel, Sample inputIndex) {
		int lowIndex = static_cast<int>(std::floor(inputIndex));
		Sample fracIndex = inputIndex - lowIndex;
		return getFractional<member>(channel, lowIndex, fracIndex);
	}
	template<Sample Band::*member>
	Sample getBand(int channel, int index) {
		if (index < 0 || index >= bands) return 0;
		return _channelBands[index + channel*bands].*member;
	}
	template<Sample Band::*member>
	Sample getFractional(int channel, int lowIndex, Sample fractional) {
		Sample low = getBand<member>(channel, lowIndex);
		Sample high = getBand<member>(channel, lowIndex + 1);
		return low + (high - low)*fractional;
	}
	template<Sample Band::*member>
	Sample getFractional(int channel, Sample inputIndex) {
		int lowIndex = std::floor(inputIndex);
		Sample fracIndex = inputIndex - lowIndex;
		return getFractional<member>(channel, lowIndex, fracIndex);
	}

	struct Peak {
		Sample input, output;
	};
	std::vector<Peak> peaks;
	std::vector<Sample> energy, smoothedEnergy;
	struct PitchMapPoint {
		Sample inputBin, freqGrad;
	};
	std::vector<PitchMapPoint> outputMap;
	
	struct Prediction {
		Sample energy = 0;
		Complex input;

		Complex makeOutput(Complex phase) {
			Sample phaseNorm = _impl::norm(phase);
			if (phaseNorm <= noiseFloor) {
				phase = input; // prediction is too weak, fall back to the input
				phaseNorm = _impl::norm(input) + noiseFloor;
			}
			return phase*std::sqrt(energy/phaseNorm);
		}
	};
	std::vector<Prediction> channelPredictions;
	Prediction * predictionsForChannel(int c) {
		return channelPredictions.data() + c*bands;
	}

	// If RandomEngine=void, use std::default_random_engine;
	using RandomEngineImpl = typename std::conditional<
		std::is_void<RandomEngine>::value,
		std::default_random_engine,
		RandomEngine
	>::type;
	RandomEngineImpl randomEngine;

	size_t processSpectrumSteps = 0;
	static constexpr size_t splitMainPrediction = 8; // it's just heavy, since we're blending up to 4 different phase predictions
	void updateProcessSpectrumSteps() {
		processSpectrumSteps = 0;
		if (blockProcess.newSpectrum) processSpectrumSteps += channels;
		if (blockProcess.mappedFrequencies) {
			processSpectrumSteps += smoothEnergySteps;
			processSpectrumSteps += 1; // findPeaks
		}
		processSpectrumSteps += 1; // updating the output map
		processSpectrumSteps += channels; // preliminary phase-vocoder prediction
		processSpectrumSteps += splitMainPrediction;
		if (blockProcess.newSpectrum) processSpectrumSteps += 1; // .input -> .prevInput
		if (blockProcess.processFormants) processSpectrumSteps += 3;
	}
	void processSpectrum(size_t step) {
		Sample timeFactor = blockProcess.timeFactor;
		
		Sample smoothingBins = Sample(stft.fftSamples())/stft.defaultInterval();
		int longVerticalStep = static_cast<int>(std::round(smoothingBins));
		timeFactor = std::max<Sample>(timeFactor, 1/maxCleanStretch);
		bool randomTimeFactor = (timeFactor > maxCleanStretch);
		std::uniform_real_distribution<Sample> timeFactorDist(maxCleanStretch*2*randomTimeFactor - timeFactor, timeFactor);

		if (blockProcess.newSpectrum) {
			if (step < size_t(channels)) {
				int channel = int(step);
				auto bins = bandsForChannel(channel);

				Complex rot = std::polar(Sample(1), bandToFreq(0)*stft.defaultInterval()*Sample(2*M_PI));
				Sample freqStep = bandToFreq(1) - bandToFreq(0);
				Complex rotStep = std::polar(Sample(1), freqStep*stft.defaultInterval()*Sample(2*M_PI));
				 
				for (int b = 0; b < bands; ++b) {
					auto &bin = bins[b];
					bin.output = _impl::mul(bin.output, rot);
					bin.prevInput = _impl::mul(bin.prevInput, rot);
					rot = _impl::mul(rot, rotStep);
				}
				return;
			}
			step -= channels;
		}
		if (blockProcess.mappedFrequencies) {
			if (step < smoothEnergySteps) {
				smoothEnergy(step, smoothingBins);
				return;
			}
			step -= smoothEnergySteps;
			if (step-- == 0) {
				findPeaks();
				return;
			}
		}
		if (step-- == 0) {
			if (blockProcess.mappedFrequencies) {
				updateOutputMap();
			} else { // we're not pitch-shifting, so no need to find peaks etc.
				for (int c = 0; c < channels; ++c) {
					Band *bins = bandsForChannel(c);
					for (int b = 0; b < bands; ++b) {
						bins[b].inputEnergy = _impl::norm(bins[b].input);
					}
				}

				for (int b = 0; b < bands; ++b) {
					outputMap[b] = {Sample(b), 1};
				}
			}
			return;
		}
		if (blockProcess.processFormants) {
			if (step < 3) {
				updateFormants(step);
				return;
			}
			step -= 3;
		}
		// Preliminary output prediction from phase-vocoder
		if (step < size_t(channels)) {
			int c = int(step);
			Band *bins = bandsForChannel(c);
			auto *predictions = predictionsForChannel(c);
			for (int b = 0; b < bands; ++b) {
				auto mapPoint = outputMap[b];
				int lowIndex = static_cast<int>(std::floor(mapPoint.inputBin));
				Sample fracIndex = mapPoint.inputBin - lowIndex;

				Prediction &prediction = predictions[b];
				Sample prevEnergy = prediction.energy;
				prediction.energy = getFractional<&Band::inputEnergy>(c, lowIndex, fracIndex);
				prediction.energy *= std::max<Sample>(0, mapPoint.freqGrad); // scale the energy according to local stretch factor
				prediction.input = getFractional<&Band::input>(c, lowIndex, fracIndex);

				auto &outputBin = bins[b];
				Complex prevInput = getFractional<&Band::prevInput>(c, lowIndex, fracIndex);
				Complex freqTwist = _impl::mul<true>(prediction.input, prevInput);
				Complex phase = _impl::mul(outputBin.output, freqTwist);
				outputBin.output = phase/(std::max(prevEnergy, prediction.energy) + noiseFloor);
			}
			return;
		}
		step -= channels;

		if (step < splitMainPrediction) {
			// Re-predict using phase differences between frequencies
			size_t chunk = step;
			int startB = int(bands*chunk/splitMainPrediction);
			int endB = int(bands*(chunk + 1)/splitMainPrediction);
			for (int b = startB; b < endB; ++b) {
				// Find maximum-energy channel and calculate that
				int maxChannel = 0;
				Sample maxEnergy = predictionsForChannel(0)[b].energy;
				for (int c = 1; c < channels; ++c) {
					Sample e = predictionsForChannel(c)[b].energy;
					if (e > maxEnergy) {
						maxChannel = c;
						maxEnergy = e;
					}
				}

				auto *predictions = predictionsForChannel(maxChannel);
				auto &prediction = predictions[b];
				auto *bins = bandsForChannel(maxChannel);
				auto &outputBin = bins[b];

				Complex phase = 0;
				auto mapPoint = outputMap[b];

				// Upwards vertical steps
				if (b > 0) {
					Sample binTimeFactor = randomTimeFactor ? timeFactorDist(randomEngine) : timeFactor;
					Complex downInput = getFractional<&Band::input>(maxChannel, mapPoint.inputBin - binTimeFactor);
					Complex shortVerticalTwist = _impl::mul<true>(prediction.input, downInput);

					auto &downBin = bins[b - 1];
					phase += _impl::mul(downBin.output, shortVerticalTwist);
					
					if (b >= longVerticalStep) {
						Complex longDownInput = getFractional<&Band::input>(maxChannel, mapPoint.inputBin - longVerticalStep*binTimeFactor);
						Complex longVerticalTwist = _impl::mul<true>(prediction.input, longDownInput);

						auto &longDownBin = bins[b - longVerticalStep];
						phase += _impl::mul(longDownBin.output, longVerticalTwist);
					}
				}
				// Downwards vertical steps
				if (b < bands - 1) {
					auto &upPrediction = predictions[b + 1];
					auto &upMapPoint = outputMap[b + 1];

					Sample binTimeFactor = randomTimeFactor ? timeFactorDist(randomEngine) : timeFactor;
					Complex downInput = getFractional<&Band::input>(maxChannel, upMapPoint.inputBin - binTimeFactor);
					Complex shortVerticalTwist = _impl::mul<true>(upPrediction.input, downInput);

					auto &upBin = bins[b + 1];
					phase += _impl::mul<true>(upBin.output, shortVerticalTwist);
					
					if (b < bands - longVerticalStep) {
						auto &longUpPrediction = predictions[b + longVerticalStep];
						auto &longUpMapPoint = outputMap[b + longVerticalStep];

						Complex longDownInput = getFractional<&Band::input>(maxChannel, longUpMapPoint.inputBin - longVerticalStep*binTimeFactor);
						Complex longVerticalTwist = _impl::mul<true>(longUpPrediction.input, longDownInput);

						auto &longUpBin = bins[b + longVerticalStep];
						phase += _impl::mul<true>(longUpBin.output, longVerticalTwist);
					}
				}

				outputBin.output = prediction.makeOutput(phase);
				
				// All other bins are locked in phase
				for (int c = 0; c < channels; ++c) {
					if (c != maxChannel) {
						auto &channelBin = bandsForChannel(c)[b];
						auto &channelPrediction = predictionsForChannel(c)[b];
						
						Complex channelTwist = _impl::mul<true>(channelPrediction.input, prediction.input);
						Complex channelPhase = _impl::mul(outputBin.output, channelTwist);
						channelBin.output = channelPrediction.makeOutput(channelPhase);
					}
				}
			}
			return;
		}
		step -= splitMainPrediction;

		if (blockProcess.newSpectrum) {
			if (step-- == 0) {
				for (auto &bin : _channelBands) {
					bin.prevInput = bin.input;
				}
			}
		}
	}
	
	// Produces smoothed energy across all channels
	static constexpr size_t smoothEnergySteps = 3;
	Sample smoothEnergyState = 0;
	void smoothEnergy(size_t step, Sample smoothingBins) {
		Sample smoothingSlew = 1/(1 + smoothingBins*Sample(0.5));
		if (step-- == 0) {
			for (auto &e : energy) e = 0;
			for (int c = 0; c < channels; ++c) {
				Band *bins = bandsForChannel(c);
				for (int b = 0; b < bands; ++b) {
					Sample e = _impl::norm(bins[b].input);
					bins[b].inputEnergy = e; // Used for interpolating prediction energy
					energy[b] += e;
				}
			}
			for (int b = 0; b < bands; ++b) {
				smoothedEnergy[b] = energy[b];
			}
			smoothEnergyState = 0;
			return;
		}

		// The two other steps are repeated smoothing passes, down and up
		Sample e = smoothEnergyState;
		for (int b = bands - 1; b >= 0; --b) {
			e += (smoothedEnergy[b] - e)*smoothingSlew;
			smoothedEnergy[b] = e;
		}
		for (int b = 0; b < bands; ++b) {
			e += (smoothedEnergy[b] - e)*smoothingSlew;
			smoothedEnergy[b] = e;
		}
		smoothEnergyState = e;
	}
	
	Sample mapFreq(Sample freq) const {
		if (customFreqMap) return customFreqMap(freq);
		if (freq > freqTonalityLimit) {
			return freq + (freqMultiplier - 1)*freqTonalityLimit;
		}
		return freq*freqMultiplier;
	}
	
	// Identifies spectral peaks using energy across all channels
	void findPeaks() {
		peaks.resize(0);
		
		int start = 0;
		while (start < bands) {
			if (energy[start] > smoothedEnergy[start]) {
				int end = start;
				Sample bandSum = 0, energySum = 0;
				while (end < bands && energy[end] > smoothedEnergy[end]) {
					bandSum += end*energy[end];
					energySum += energy[end];
					++end;
				}
				Sample avgBand = bandSum/energySum;
				Sample avgFreq = bandToFreq(avgBand);
				peaks.emplace_back(Peak{avgBand, freqToBand(mapFreq(avgFreq))});

				start = end;
			}
			++start;
		}
	}
	
	void updateOutputMap() {
		if (peaks.empty()) {
			for (int b = 0; b < bands; ++b) {
				outputMap[b] = {Sample(b), 1};
			}
			return;
		}
		Sample bottomOffset = peaks[0].input - peaks[0].output;
		for (int b = 0; b < std::min(bands, static_cast<int>(std::ceil(peaks[0].output))); ++b) {
			outputMap[b] = {b + bottomOffset, 1};
		}
		// Interpolate between points
		for (size_t p = 1; p < peaks.size(); ++p) {
			const Peak &prev = peaks[p - 1], &next = peaks[p];
			Sample rangeScale = 1/(next.output - prev.output);
			Sample outOffset = prev.input - prev.output;
			Sample outScale = next.input - next.output - prev.input + prev.output;
			Sample gradScale = outScale*rangeScale;
			int startBin = std::max(0, static_cast<int>(std::ceil(prev.output)));
			int endBin = std::min(bands, static_cast<int>(std::ceil(next.output)));
			for (int b = startBin; b < endBin; ++b) {
				Sample r = (b - prev.output)*rangeScale;
				Sample h = r*r*(3 - 2*r);
				Sample outB = b + outOffset + h*outScale;
				
				Sample gradH = 6*r*(1 - r);
				Sample gradB = 1 + gradH*gradScale;
				
				outputMap[b] = {outB, gradB};
			}
		}
		Sample topOffset = peaks.back().input - peaks.back().output;
		for (int b = std::max(0, static_cast<int>(peaks.back().output)); b < bands; ++b) {
			outputMap[b] = {b + topOffset, 1};
		}
	}

	// If we mapped formants the same way as mapFreq(), this would be the inverse
	Sample invMapFormant(Sample freq) const {
		if (freq*invFormantMultiplier > freqTonalityLimit) {
			return freq + (1 - formantMultiplier)*freqTonalityLimit;
		}
		return freq*invFormantMultiplier;
	}

	Sample freqEstimateWeighted = 0;
	Sample freqEstimateWeight = 0;
	Sample estimateFrequency() {
		// 3 highest peaks in the input
		std::array<int, 3> peakIndices{0, 0, 0};
		for (int b = 1; b < bands - 1; ++b) {
			Sample e = formantMetric[b];
			// local maxima only
			if (e < formantMetric[b - 1] || e <= formantMetric[b + 1]) continue;
			
			if (e > formantMetric[peakIndices[0]]) {
				if (e > formantMetric[peakIndices[1]]) {
					if (e > formantMetric[peakIndices[2]]) {
						peakIndices = {peakIndices[1], peakIndices[2], b};
					} else {
						peakIndices = {peakIndices[1], b, peakIndices[2]};
					}
				} else {
					peakIndices[0] = b;
				}
			}
		}
		
		// VERY rough pitch estimation
		int peakEstimate = peakIndices[2];
		if (formantMetric[peakIndices[1]] > formantMetric[peakIndices[2]]*0.1) {
			int diff = std::abs(peakEstimate - peakIndices[1]);
			if (diff > peakEstimate/8 && diff < peakEstimate*7/8) peakEstimate = peakEstimate%diff;
			if (formantMetric[peakIndices[0]] > formantMetric[peakIndices[2]]*0.01) {
				diff = std::abs(peakEstimate - peakIndices[0]);
				if (diff > peakEstimate/8 && diff < peakEstimate*7/8) peakEstimate = peakEstimate%diff;
			}
		}
		Sample weight = formantMetric[peakIndices[2]];
		// Smooth it out a bit
		freqEstimateWeighted += (peakEstimate*weight - freqEstimateWeighted)*Sample(0.25);
		freqEstimateWeight += (weight - freqEstimateWeight)*Sample(0.25);
		
		return freqEstimateWeighted/(freqEstimateWeight + Sample(1e-30));
	}
	
	Sample freqEstimate;
	
	std::vector<Sample> formantMetric;
	Sample formantBaseFreq = 0;
	void updateFormants(size_t step) {
		if (step-- == 0) {
			for (auto &e : formantMetric) e = 0;
			for (int c = 0; c < channels; ++c) {
				Band *bins = bandsForChannel(c);
				for (int b = 0; b < bands; ++b) {
					formantMetric[b] += bins[b].inputEnergy;
				}
			}

			freqEstimate = freqToBand(formantBaseFreq);
			if (formantBaseFreq <= 0) freqEstimate = estimateFrequency();
		} else if (step-- == 0) {
			Sample decay = 1 - 1/(freqEstimate*Sample(0.5) + 1);
			Sample e = 0;
			for (size_t repeat = 0; repeat < 2; ++repeat) {
				for (int b = bands - 1; b >= 0; --b) {
					e = std::max(formantMetric[b], e*decay);
					formantMetric[b] = e;
				}
				for (int b = 0; b < bands; ++b) {
					e = std::max(formantMetric[b], e*decay);
					formantMetric[b] = e;
				}
			}
			decay = 1/decay;
			for (size_t repeat = 0; repeat < 2; ++repeat) {
				for (int b = bands - 1; b >= 0; --b) {
					e = std::min(formantMetric[b], e*decay);
					formantMetric[b] = e;
				}
				for (int b = 0; b < bands; ++b) {
					e = std::min(formantMetric[b], e*decay);
					formantMetric[b] = e;
				}
			}
		} else {
			auto getFormant = [&](Sample band) -> Sample {
				if (band < 0) return 0;
				band = std::min(band, static_cast<Sample>(bands));
				int floorBand = std::floor(band);
				Sample fracBand = band - floorBand;
				Sample low = formantMetric[floorBand], high = formantMetric[floorBand + 1];
				return low + (high - low)*fracBand;
			};

			for (int b = 0; b < bands; ++b) {
				Sample inputF = bandToFreq(static_cast<Sample>(b));
				Sample outputF = formantCompensation ? mapFreq(inputF) : inputF;
				outputF = invMapFormant(outputF);

				Sample inputE = formantMetric[b];
				Sample targetE = getFormant(freqToBand(outputF));

				Sample formantRatio = targetE/(inputE + Sample(1e-30));
				Sample energyRatio = formantRatio;

				for (int c = 0; c < channels; ++c) {
					Band *bins = bandsForChannel(c);
					// This is what's used to decide the output energy, so this affects the output
					bins[b].inputEnergy *= energyRatio;
				}
			}
		}
	}

	// Proxy class to avoid copying/allocating anything
	template<class Io>
	struct OffsetIO {
		Io &io;
		int offset;

		struct Channel {
			Io &io;
			int channel;
			int offset;
			
			auto operator[](int i) -> decltype(io[0][0]) {
				return io[channel][i + offset];
			}
		};
		Channel operator[](int c) {
			return {io, c, offset};
		}
	};
};

}} // namespace
#endif // include guard
