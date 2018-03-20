#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <thread>
#include <cmath>
#include <chrono>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/ini_parser.hpp>
#include <log4cpp/Category.hh>
#include <log4cpp/FileAppender.hh>
#include <log4cpp/OstreamAppender.hh>
#include <portaudio.h>
#include <mumlib/Transport.hpp>
#include "MumpiCallback.hpp"
#include "RingBuffer.hpp"

const int NUM_CHANNELS = 1;

static log4cpp::Appender *appender = new log4cpp::OstreamAppender("console", &std::cout);
static log4cpp::Category& logger = log4cpp::Category::getRoot();
static volatile sig_atomic_t sig_caught = 0;
// static bool mumble_thread_run_flag = true;
// static bool input_consumer_thread_run_flag = true;

/**
 * Signal interrupt handler
 *
 * @param signal the signal
 */
static void sigHandler(int signal) {
	logger.warn("caught signal %d", signal);
	sig_caught = signal;
}

/**
 * Simple data structure for storing audio sample data
 */
struct PaData {
	std::shared_ptr<RingBuffer<int16_t>> rec_buf;	// recording ring buffer
	std::shared_ptr<RingBuffer<int16_t>> out_buf;	// output ring buffer
};

/**
 * Record callback for PortAudio engine. This gets called when audio input is
 * available. This will simply take the data from the input buffer and store
 * it in a ring buffer that we allocated. That data will then be consumed by
 * another thread.
 *
 * @param  inputBuffer     input sample buffer (interleaved if multi channel)
 * @param  outputBuffer    output sample buffer (interleaved if multi channel)
 * @param  framesPerBuffer number of frames per buffer
 * @param  timeInfo        time information for stream
 * @param  statusFlags     i/o buffer status flags
 * @param  userData        circular buffer
 * @return                 PaStreamCallbackResult, paContinue usually
 */
static int paRecordCallback(const void *inputBuffer,
                            void *outputBuffer,
                            unsigned long framesPerBuffer,
                            const PaStreamCallbackTimeInfo* timeInfo,
                            PaStreamCallbackFlags statusFlags,
                            void *userData ) {
	int result = paContinue;
	// cast the pointers to the appropriate types
	const PaData *pa_data = (const PaData*) userData;
	int16_t *input_buffer = (int16_t*) inputBuffer;
	(void) outputBuffer;
	(void) timeInfo;
	(void) statusFlags;

	if(inputBuffer != NULL) {
		// fill ring buffer with samples
		pa_data->rec_buf->push(input_buffer, 0, framesPerBuffer * NUM_CHANNELS);
	} else {
		// fill ring buffer with silence
		for(int i = 0; i < framesPerBuffer * NUM_CHANNELS; i += NUM_CHANNELS) {
			for(int j = 0; j < NUM_CHANNELS; j++) {
				pa_data->rec_buf->push(0);
			}
		}
	}

	return result;
}

/**
 * Output callback for PortAudio engine. This gets called when audio output is
 * ready to be sent. This will simply consume data from a ring buffer that we
 * allocated which is being filled by another thread.
 *
 * @param  inputBuffer     input sample buffer (interleaved if multi channel)
 * @param  outputBuffer    output sample buffer (interleaved if multi channel)
 * @param  framesPerBuffer number of frames per buffer
 * @param  timeInfo        time information for stream
 * @param  statusFlags     i/o buffer status flags
 * @param  userData        circular buffer
 * @return                 PaStreamCallbackResult, paContinue usually
 */
static int paOutputCallback(const void *inputBuffer,
                            void *outputBuffer,
                            unsigned long framesPerBuffer,
                            const PaStreamCallbackTimeInfo* timeInfo,
                            PaStreamCallbackFlags statusFlags,
                            void *userData ) {
	int result = paContinue;
	// cast the pointers to the appropriate types
	const PaData *pa_data = (const PaData*) userData;
	int16_t *output_buffer = (int16_t*) outputBuffer;
	(void) inputBuffer;
	(void) timeInfo;
	(void) framesPerBuffer;
	(void) statusFlags;

	// output pcm data to PortAudio's output_buffer by reading from our ring buffer
	// if we dont have enough samples in our ring buffer, we have to still supply 0s to the output_buffer
	const size_t requested_samples = (framesPerBuffer * NUM_CHANNELS);
	const size_t available_samples = pa_data->out_buf->getRemaining();	
	logger.info("requested_samples: %d", requested_samples);
	logger.info("available_samples: %d", available_samples);
	if(requested_samples > available_samples) {
		pa_data->out_buf->top(output_buffer, 0, available_samples);
		for(size_t i = available_samples; i < requested_samples - available_samples; i++) {
			output_buffer[i] = 0;
		}
	} else {
		pa_data->out_buf->top(output_buffer, 0, requested_samples);
	}

	return result;
}

/**
 * Gets the next power of 2 for the passed argument
 *
 * @param  val input value
 * @return     next power of 2 for passed arg
 */
static unsigned nextPowerOf2(unsigned val) {
    val--;
    val = (val >> 1) | val;
    val = (val >> 2) | val;
    val = (val >> 4) | val;
    val = (val >> 8) | val;
    val = (val >> 16) | val;
    return ++val;
}
/*
 * Change device master volume
 * @param valume	input volume
*/
void setAlsaMasterVolume(long volume)
{
	std::stringstream command;
	command << "amixer -D pulse sset Master " << volume << "%";
	// if (volume >= 0 && volume <= 100) {
	// 	system(command.str().c_str());
	// } else {
	// 	logger.error("Sound volume between 0 - 100");
	// }
}

/**
 * main function
 *
 * Program flow:
 * 1. Parse file configuration
 * 2. Init PortAudio engine and open default input and output audio devices
 * 3. Init mumlib client
 * 4. Busy loop until CTRL+C
 * 5. Clean up mumlib client
 * 6. Clean up PortAudio engine
 */
int main(int argc, char *argv[]) {
	bool verbose = false;
	std::string server;
	int port;
	std::string username;
	std::string password;
	int next_option;
	int sample_rate;
	int frame_per_buffer;
	double output_delay = -1.0;
	double vox_threshold;	// dB
	std::chrono::duration<double> voice_hold_interval(0.050);	// 50 ms
	bool voice_target_flag = false;
	int voice_target = 0;

	// init logger
	appender->setLayout(new log4cpp::BasicLayout());
	logger.setPriority(log4cpp::Priority::WARN);
	logger.addAppender(appender);

	///////////////////////
	// get configuration
	///////////////////////
	boost::property_tree::ptree pt;
	boost::property_tree::ini_parser::read_ini("configuration.ini", pt);

	server 		= pt.get<std::string>("Server.host", "192.168.2.107");
	port 		= pt.get<int>("Server.port", 64738);
	username	= pt.get<std::string>("Server.username", "Client1");
	verbose 	= pt.get<bool>("Client.debug", false);
	sample_rate = pt.get<int>("Client.sampleRate", 48000);
	frame_per_buffer	= pt.get<int>("Client.framePerBuffer", 128);
	vox_threshold 		= pt.get<double>("Client.voxThreshold", -90);

	if(verbose)
		logger.setPriority(log4cpp::Priority::INFO);
	
	// check for valid sample rate
	if(sample_rate != 48000 && sample_rate != 24000 && sample_rate != 12000) {
		logger.error("--sample-rate option must be 12000, 24000, or 48000");
		exit(-1);
	}
	logger.info("======================================================");
	logger.info("Server:        %s", server.c_str());
	logger.info("Port:			%d", port);
	logger.info("Username:      %s", username.c_str());
	logger.info("sample rate    %d", sample_rate);
	logger.info("vox threshold  %f", vox_threshold);
	logger.info("======================================================");

	///////////////////////
	// init audio library
	///////////////////////
	PaError err;
	err = Pa_Initialize();
	if(err != paNoError) {
		logger.error("PortAudio error: %s", Pa_GetErrorText(err));
		exit(-1);
	}

	logger.info(Pa_GetVersionText());

	// init audio I/O streams
	PaStream *input_stream;
	PaStream *output_stream;
	PaData data;
	PaStreamParameters inputParameters;
	PaStreamParameters outputParameters;

	// set ring buffer size to about 500ms
	const size_t MAX_SAMPLES = nextPowerOf2(0.5 * sample_rate * NUM_CHANNELS);
	data.rec_buf = std::make_shared<RingBuffer<int16_t>>(MAX_SAMPLES);
	data.out_buf = std::make_shared<RingBuffer<int16_t>>(MAX_SAMPLES);

	inputParameters.device = Pa_GetDefaultInputDevice();
	if (inputParameters.device == paNoDevice) {
		logger.error("No default input device.");
		exit(-1);
	}
	inputParameters.channelCount = NUM_CHANNELS;
	inputParameters.sampleFormat = paInt16;
	inputParameters.suggestedLatency = Pa_GetDeviceInfo(inputParameters.device)->defaultLowInputLatency;
	inputParameters.hostApiSpecificStreamInfo = NULL;

	logger.info("inputParameters.suggestedLatency: %.4f", inputParameters.suggestedLatency);

	err = Pa_OpenStream(&input_stream,         // the input stream
						&inputParameters,      // input params
						NULL,                  // output params
						sample_rate,           // sample rate
						frame_per_buffer,     // frames per buffer
						paClipOff,             // we won't output out of range samples so don't bother clipping them
						paRecordCallback,      // PortAudio callback function
						&data);                // data pointer

	logger.info("defaultHighOutputLatency: %.4f", Pa_GetDeviceInfo(inputParameters.device)->defaultHighOutputLatency);

	if(err != paNoError) {
	    logger.error("Failed to open input stream: %s", Pa_GetErrorText(err));
		exit(-1);
	}

	outputParameters.device = Pa_GetDefaultOutputDevice();
	if(outputParameters.device == paNoDevice) {
		logger.error("No default output device.");
		exit(-1);
	}
	outputParameters.channelCount = NUM_CHANNELS;
	outputParameters.sampleFormat =  paInt16;

	if(output_delay < 0.0)
		output_delay = Pa_GetDeviceInfo(outputParameters.device)->defaultHighOutputLatency;
	outputParameters.suggestedLatency = output_delay;
	outputParameters.hostApiSpecificStreamInfo = NULL;

	logger.info("outputParameters.suggestedLatency: %.4f", outputParameters.suggestedLatency);

	err = Pa_OpenStream(&output_stream,		// the output stream
						NULL, 				// input params
						&outputParameters,	// output params
						sample_rate,		// sample rate
						frame_per_buffer,	// frames per buffer
						paClipOff,      	// we won't output out of range samples so don't bother clipping them
						paOutputCallback,	// PortAudio callback function
						&data);				// data pointer

	logger.info("defaultHighOutputLatency: %.4f", Pa_GetDeviceInfo(outputParameters.device)->defaultHighOutputLatency);

	if(err != paNoError) {
		logger.error("Failed to open output stream: %s", Pa_GetErrorText(err));
		exit(-1);
	}

	// start the streams
	err = Pa_StartStream(input_stream);
	if(err != paNoError) {
		logger.error("Failed to start input stream: %s", Pa_GetErrorText(err));
		exit(-1);
	}

	err = Pa_StartStream(output_stream);
	if(err != paNoError) {
		logger.error("Failed to start output stream: %s", Pa_GetErrorText(err));
		exit(-1);
	}

	///////////////////////
	// init mumble library
	///////////////////////
	// open output audio stream, pipe incoming audio PCM data to output audio stream

	// This stuff should be on a separate thread
	MumpiCallback mumble_callback(data.out_buf);
	mumlib::MumlibConfiguration conf;
	conf.opusEncoderBitrate = sample_rate;
	mumlib::Mumlib mum(mumble_callback, conf);
	mumble_callback.mum = &mum;

	std::thread mumble_main_thread([&]() {		
		while(!sig_caught) {			
			try {
				logger.info("Connecting to %s", server.c_str());
				mum.connect(server, port, username, password);
				mum.run();
			}catch (mumlib::TransportException &exp) {
				logger.error("TransportException: %s.", exp.what());
				logger.error("Attempting to reconnect in 5 s.");
				// mum.reconnect();
				std::this_thread::sleep_for(std::chrono::seconds(5));
			}
		}
	});
	std::thread mumble_transmitting_thread([&]() {
		// consumes the data that the input audio thread receives and sends it
		// through mumble client
		// this will continuously read from the input data circular buffer

		// Opus can encode frames of 2.5, 5, 10, 20, 40, or 60 ms
		// the Opus RFC 6716 recommends using 20ms frame sizes
		// so at 48k sample rate, 20ms is 960 samples
		const int OPUS_FRAME_SIZE = (sample_rate / 1000.0)*20.0;

		logger.info("OPUS_FRAME_SIZE: %d", OPUS_FRAME_SIZE);

		std::chrono::steady_clock::time_point start;
		std::chrono::steady_clock::time_point now;
		bool voice_hold_flag = false;
		bool first_run_flag = true;
		int16_t *out_buf = new int16_t[MAX_SAMPLES];
		while(!sig_caught) {
			if(!data.rec_buf->isEmpty() && data.rec_buf->getRemaining() >= OPUS_FRAME_SIZE) {

				// perform VOX algorithm
				// convert each sample to dB
				// take average (RMS) of all samples
				// if average >= threshold, transmit, else ignore
				// dB = 20 * log_10(rms)

				// also perform a "voice hold" for aprox voice_hold_interval
				// if we have just transmitted

				// do a bulk get and send it through mumble client
				if(mum.getConnectionState() == mumlib::ConnectionState::CONNECTED) {
					data.rec_buf->top(out_buf, 0, OPUS_FRAME_SIZE);

					// compute RMS of sample window
					double sum = 0;
					for(int i = 0; i < OPUS_FRAME_SIZE; i++) {
						const double sample = std::abs(out_buf[i]) / INT16_MAX;
						sum += sample * sample;
					}
					const double rms = std::sqrt(sum / OPUS_FRAME_SIZE);
					
					double db = vox_threshold;
					if(rms > 0.0)
						db = 20.0 * std::log10(rms);

					logger.info("Recorded voice dB: %.2f", db);

					if(!first_run_flag) {
						now = std::chrono::steady_clock::now();
						auto duration = now - start;
						if(duration < voice_hold_interval)
							voice_hold_flag = true;
						else
							voice_hold_flag = false;
					}

					if(db >= vox_threshold || voice_hold_flag)	{ // only tx if vox threshold met
						// if(voice_target_flag)
						// 	mum.sendAudioDataTarget(voice_target, out_buf, OPUS_FRAME_SIZE);
						// else 
						mum.sendAudioData(out_buf, OPUS_FRAME_SIZE);					

						if(!voice_hold_flag) {
							start = std::chrono::steady_clock::now();
							first_run_flag = false;
						}
					}
				}
			} else {
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
			}
		}
		delete[] out_buf;
	});

	std::thread mumble_gpio_thread([&]() {
		while(!sig_caught) {
			// int mumbleChannels;
			// std::cout << "Change user chanels :";
			// std::cin >> mumbleChannels;
			// mum.joinChannel(mumbleChannels);
			if (mum.getConnectionState() == mumlib::ConnectionState::CONNECTED) {
				int message;
				std::cout << "Send messages:";
 				std::cin >> message;
				// mum.sendTextMessage("sen message long very long long long");
				// mum.selfDeaf(true);
				// mum.joinChannel(1);
				// mum.joinChannel(message);
				// mum.sendVoiceTarget(1, message);
				// setAlsaMasterVolume(message);
			}
		}
	});

	// init signal handler
	struct sigaction action;
	action.sa_handler = sigHandler;
	action.sa_flags = 0;
	sigemptyset(&action.sa_mask);
	sigaction(SIGINT, &action, NULL);
	sigaction(SIGTERM, &action, NULL);

	// busy loop until signal is caught
	while(!sig_caught) {
		std::this_thread::sleep_for(std::chrono::milliseconds(250));
	}

	///////////////////////
	// CLEAN UP
	///////////////////////
	logger.warn("Cleaning up...");
	
	///////////////////////////
	// clean up mumble library
	///////////////////////////
	logger.warn("Disconnecting...");
	
	if(mumble_transmitting_thread.joinable()) {
		logger.warn("Join audio thread");
		mumble_transmitting_thread.join();
	}
	if(mumble_gpio_thread.joinable()) {
		logger.warn("Join gpio thread");
		mumble_gpio_thread.join();
	}

	mum.disconnect();
	if(mumble_main_thread.joinable()) {
		logger.warn("Join main thread");
		mumble_main_thread.join();
	}
	
	///////////////////////////
	// clean up audio library
	///////////////////////////
	logger.warn("Cleaning up PortAudio...");

	// close streams
	logger.warn("Closing input stream");
	err = Pa_CloseStream(input_stream);
	if(err != paNoError) {
		logger.error("Failed to close inputstream: %s", Pa_GetErrorText(err));
		exit(-1);
	}
	logger.warn("Closing output stream");
	err = Pa_CloseStream(output_stream);
	if(err != paNoError) {
		logger.error("Failed to close output stream: %s", Pa_GetErrorText(err));
		exit(-1);
	}

	// terminate PortAudio engine
	logger.warn("Terminating PortAudio engine");
	err = Pa_Terminate();
	if(err != paNoError) {
		logger.error("PortAudio error: %s", Pa_GetErrorText(err));
		exit(-1);
	}

	return 0;
}
