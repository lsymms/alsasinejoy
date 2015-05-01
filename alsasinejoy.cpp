#include <fcntl.h>
#include <iostream>
#include <math.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <thread>
#include <time.h> 
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/joystick.h>
#include <linux/soundcard.h>
#include "alsa/asoundlib.h"
#include "alsa/mixer.h"
#include "wavepatterns.h"
//#include <libconfig.h++>

//using namespace libconfig;
using namespace std;

#define DEGREE_TO_RADIANS = 0.00872664
const int max_buf_size = 1024;


int dsp_out_fd;
int joy_fd;
snd_pcm_t *alsa_handle;
int sample_rate = 48000;
int sinebuf_size = 48000;
int use_dsp = false;
static bool keepRunning = true;
char *joy_dev = "/dev/input/js0";
char *snd_dev = "default";
float wave_scale = 255.0/2;
int wave_midpoint = 127;
int wave_midpoint_precision = 5;
int joystick_initialized = 0;
int joystick_detected = 0;
int freq_axis_val = 0;
bool muted = true;
int initial_freq_axis_val;
int latency = 100000; // .1 sec in us
int buffer_size = 64;
int freq_step = 2;  
// initial frequency  
int center_freq = 7900;
/* factor of 65534 which determines freq range 
  	(e.g. range 4 means a freq range of 16383 Hz) */
float range = 4;
int range_min = 1;
int range_max = 256;
int disp_live_values = true;

//initial volume (0-100)
float initial_vol = 30;
//number to divide the vol_axis_val by to get the volume
float calc_vol_multiplier() { return 1/(( 65534/((100-initial_vol) /100) ) / 100); }
float vol_multiplier = calc_vol_multiplier();
int initial_vol_axis_val;
int vol_axis_val;
//number to multiply the volume by to scale to raw value
float vol_pct_scale;
long min_vol_raw_val;
long vol_on_entry;
snd_mixer_t* mixer_handle;
snd_mixer_elem_t* mixer_elem_handle;
const char *mixer_name = "PCM";

// low duty cycle vars
int ldc_freq = 500;
unsigned char * ldc_table;
int ldc_duty_cycle_pct = 15;
int ldc_samples;

// lowest & highest frequencies allowed (speaker protection)  
int freq_floor = 20;
int freq_ceiling = 20000;

int joystick_update_every_us = 1000;

// button definitions
const int freq_up_but = 1;
const int freq_dn_but = 3;
const int range_up_but = 0;
const int range_dn_but = 4;
const int vol_up_but = 2;
const int vol_dn_but = 5;
const int mute_but = 6;
const int unmute_but = 7;

// axis definitions
int freq_axis_arg = 1;
int vol_axis_arg = 6;

static unsigned char *flatline;

static unsigned char *alsa_cur_buf = alsa_sine_buf;

void printHeader() {
	if (disp_live_values) {
		printf("Freq Axis   Axis Delta   Vol Axis    Vol Delta    Center_Freq    Freqency     Volume%\n");
	}
}

void mute() {
	if (muted != true) {
		muted = true;
		printf ("\nmuted\n");
		printHeader();
	}
}

void unmute() {
	if (muted != false) {
		muted = false;
		printf ("\nunmuted\n");
		printHeader();
	}
}

void init_ldc_table(int duty_cycle_pct) {
	ldc_samples = sample_rate / ldc_freq;
	int on_samples = ldc_samples * duty_cycle_pct / 100;
	ldc_table = (unsigned char *) calloc( ldc_samples, sizeof( unsigned char ) );
	
	printf ("ldc_samples: %d\nldc_on_samples: %d\nldc table: ", ldc_samples, on_samples);
	for (int i=0; i < ldc_samples; i++) {
		if (i < on_samples)
			ldc_table[i] = 0xFF;
		else
			ldc_table[i] = 0x00;
	}
	for (int j=0; j < ldc_samples; j++) {
		printf ("%d ", ldc_table[j]);
	}
	printf ("\n");
}

static void setAlsaMasterVolumeRaw (long volumeRaw) {
    snd_mixer_selem_set_playback_volume_all(mixer_elem_handle, volumeRaw);
}

static void setAlsaMasterVolume(long volume) {
    setAlsaMasterVolumeRaw((volume * vol_pct_scale)+min_vol_raw_val);
}


void stopHandler(int dummy=0) {
  keepRunning = false;
  printf("Closing Devices\n");
  
	setAlsaMasterVolumeRaw(vol_on_entry);
	snd_mixer_close(mixer_handle);	
  if (use_dsp) {
  	close( dsp_out_fd );
	} else {		
		snd_pcm_drop(alsa_handle);
		snd_pcm_close(alsa_handle);
	}
	close( joy_fd );
}



static int calc_step (int current_freq) {
	return current_freq * sinebuf_size / sample_rate;
}

static int calc_steps (int step) {
	return sinebuf_size / step;
}



static void write_alsa(unsigned char phase_buf[], int phase_buf_size) {
	int err;
  snd_pcm_sframes_t frames = snd_pcm_writei(alsa_handle, phase_buf, phase_buf_size);
  if (frames < 0) {
    fprintf(stderr, "\n<<<<<<<<<<<<<<< Buffer Underrun >>>>>>>>>>>>>>>\n");
  	frames = snd_pcm_prepare(alsa_handle);
  }
  
  if (frames < 0) {
  	printf ("\n\nrecover\n");
    frames = snd_pcm_recover(alsa_handle, frames, 0);
  }
  
  if (frames < 0) {
    printf("\n\nsnd_pcm_writei failed: %s\n", snd_strerror(err));
    stopHandler();
  }
  
  if (frames > 0 && frames < phase_buf_size) {
    printf("\n\nShort write (expected %li, wrote %li)\n", (long)phase_buf_size, frames);
	}
}

static void write_dsp(unsigned char phase_buf[], int buf_size) {
	
  if (write (dsp_out_fd, phase_buf, buf_size) != buf_size) {
    perror ("\n\nAudio write");
   	stopHandler();
  }
} 


// TODO: read freq from global written to by joystick thread
static void write_wave_from_table(int current_freq, float cur_vol) {
	// phase is the location in the sine table
	static int phase = 0;
	static int ldc_phase = 0;
	static int pulse_phase = 0;
	static unsigned char *cycle_buf =(unsigned char *) calloc( buffer_size, sizeof( unsigned char ) );
	
	for (int i=0; i < buffer_size; i++) {
		if (use_dsp) {
			cycle_buf[i] = dsp_sine_buf[phase];
		} else {					
			cycle_buf[i] = alsa_cur_buf[phase]*cur_vol/100 * pulse_buf[pulse_phase];
//			cycle_buf[i] = alsa_cur_buf[phase] & ldc_table[ldc_phase];
		}	
		phase += current_freq;
		if (phase >= sinebuf_size) {
			phase = phase - sinebuf_size;
		}

		pulse_phase += 56;
		if (pulse_phase >= sinebuf_size) {
			pulse_phase = pulse_phase - sinebuf_size;
		}

/*		ldc_phase++;
		if (ldc_phase == ldc_samples) {
			ldc_phase = 0;
		}*/
	}
	
	if (use_dsp) {
		if (!muted) {		
	 		write_dsp(cycle_buf, buffer_size);
	 	} else {
	 		write_dsp(flatline, buffer_size);
	 	}	 			
	} else {
		if (!muted) {		
	 	 	write_alsa(cycle_buf, buffer_size);
	 	} else {
	 		write_alsa(flatline, buffer_size);
	  } 
	}
}	
	
	
static void initializeMixer() {
    long max;
    snd_mixer_selem_id_t *sid;
    const char *card = "default";
    
    snd_mixer_open(&mixer_handle, 0);
    snd_mixer_attach(mixer_handle, card);
    snd_mixer_selem_register(mixer_handle, NULL, NULL);
    snd_mixer_load(mixer_handle);

    snd_mixer_selem_id_alloca(&sid);
    snd_mixer_selem_id_set_index(sid, 0);
    snd_mixer_selem_id_set_name(sid, mixer_name);
    mixer_elem_handle = snd_mixer_find_selem(mixer_handle, sid);

    snd_mixer_selem_get_playback_volume_range(mixer_elem_handle, &min_vol_raw_val, &max);
		vol_pct_scale = (max-min_vol_raw_val) / 100;
		// you have to grab a specific channel
		if(snd_mixer_selem_get_playback_volume(mixer_elem_handle, SND_MIXER_SCHN_FRONT_LEFT, &vol_on_entry) < 0) {
			exit(-1);
		}
		printf("volume on entry: %d\n",vol_on_entry);
}


// The initialize_dsp opens the audio device and initializes it for the required mode. 
static void initialize_dsp (char *name) {
  int tmp, fd;

	printf ("initializing alsa driver\n");
	
  if ((fd = open (name, O_WRONLY, 0)) == -1)
    {
      perror (name);
      exit (-1);
    }



// Setup the device. Note that it's important to set the sample format, number of channels and sample rate exactly in this order. Some devices depend on the order.
// 
//  
// 
// 
// Set the sample format

   tmp = AFMT_S16_NE;		/* Native 16 bits */
  if (ioctl (fd, SNDCTL_DSP_SETFMT, &tmp) == -1)
    {
      perror ("SNDCTL_DSP_SETFMT");
      exit (-1);
    }

  if (tmp != AFMT_S16_NE)
    {
      fprintf (stderr,
	       "The device doesn't support the 16 bit sample format.\n");
      exit (-1);
    }



// Set the number of channels

   tmp = 1;
  if (ioctl (fd, SNDCTL_DSP_CHANNELS, &tmp) == -1)
    {
      perror ("SNDCTL_DSP_CHANNELS");
      exit (-1);
    }

  if (tmp != 1)
    {
      fprintf (stderr, "The device doesn't support mono mode.\n");
      exit (-1);
    }



// Set the sample rate

  if (ioctl (fd, SNDCTL_DSP_SPEED, &sample_rate) == -1)
    {
      perror ("SNDCTL_DSP_SPEED");
      exit (-1);
    }



// No need for error checking because we will automatically adjust the signal based on the actual sample rate. However most application must check the value of sample_rate and compare it to the requested rate.
// 
// Small differences between the rates (10% or less) are normal and the applications should usually tolerate them. However larger differences may cause annoying pitch problems (Mickey Mouse).


  dsp_out_fd = fd;
}

static void initialize_alsa(char *snd_dev) {
	
	printf ("initializing alsa driver\n");
  int err;
  
	if ((err = snd_pcm_open(&alsa_handle, snd_dev, SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
	  printf("Playback open error: %s\n", snd_strerror(err));
	  exit(EXIT_FAILURE);
	}
	if ((err = snd_pcm_set_params(alsa_handle,
	                              SND_PCM_FORMAT_U8,
	                              SND_PCM_ACCESS_RW_INTERLEAVED,
	                              1,
	                              sample_rate,
	                              1,
	                              latency)) < 0) {   /* buffer rep in sec */
	  printf("Playback open error: %s\n", snd_strerror(err));
	  exit(EXIT_FAILURE);
	}
	initializeMixer();
	setAlsaMasterVolume(100);
	printf("finished initializing alsa\n");
}	    

void joystick_thread() {
	int *axis=NULL, num_of_axis=0, num_of_buttons=0, x, i;
	char *button=NULL, name_of_joystick[80];
	struct js_event js;
	int freq_axis_ndx = freq_axis_arg-1;
	int vol_axis_ndx = vol_axis_arg-1;
	bool lock_range_up = false;
	bool lock_range_dn = false;
	bool lock_vol_up = false;
	bool lock_vol_dn = false;
	

	
	if( ( joy_fd = open( joy_dev , O_RDONLY)) == -1 )
	{
		printf( "Couldn't open joystick at %s\n", joy_dev );
	  stopHandler();
	}

	ioctl( joy_fd, JSIOCGAXES, &num_of_axis );
	ioctl( joy_fd, JSIOCGBUTTONS, &num_of_buttons );
	ioctl( joy_fd, JSIOCGNAME(80), &name_of_joystick );


	
	if (freq_axis_arg > num_of_axis) {
		printf("invalid freq axis number: %i. Joystick only has %i axes", freq_axis_arg, num_of_axis);
		stopHandler();
	}
	
  if (vol_axis_arg > num_of_axis) {
		printf("invalid vol axis number: %i. Joystick only has %i axes", vol_axis_arg, num_of_axis);
		stopHandler();
	} 
	
	axis = (int *) calloc( num_of_axis, sizeof( int ) );
	button = (char *) calloc( num_of_buttons, sizeof( char ) );
	


	fcntl( joy_fd, F_SETFL, O_NONBLOCK );	/* use non-blocking mode */

	joystick_detected = 1;
  
  printf("Joystick detected: %s\n\t%d axis\n\t%d buttons\n\nSettings\n\tcontrol axis: %d\n\tcontrol axis: %d\n\n"
		, name_of_joystick
		, num_of_axis
		, num_of_buttons
		, freq_axis_arg
		, vol_axis_arg );
	
	while (joystick_initialized == 0)
	{
		/* read the joystick state */
		read(joy_fd, &js, sizeof(struct js_event));
		
		if ((js.type & ~JS_EVENT_INIT) == JS_EVENT_AXIS && js.number == freq_axis_ndx) {
			initial_freq_axis_val = freq_axis_val = js.value;			
			initial_vol_axis_val = vol_axis_val = -32767;
			joystick_initialized = 1;
		}
	}
	
	printf ("\nJoystick Initialized\n");
	
	while (keepRunning) {
		/* read the joystick state */
		read(joy_fd, &js, sizeof(struct js_event));
		
		if ((js.type & ~JS_EVENT_INIT) == JS_EVENT_AXIS && js.number == freq_axis_ndx) {
			freq_axis_val = js.value;			
		} else if ((js.type & ~JS_EVENT_INIT) == JS_EVENT_AXIS && js.number == vol_axis_ndx) {
			vol_axis_val = js.value;
		} else if ((js.type & ~JS_EVENT_INIT) == JS_EVENT_BUTTON) {
			if (js.value == 0) {
				switch(js.number) {
					case vol_up_but:
						lock_vol_up=false;
						break;
					case vol_dn_but:
						lock_vol_dn=false;
						break;
					case range_up_but:
						lock_range_up=false;
						break;
					case range_dn_but:
						lock_range_dn=false;
						break;	
				}
			} else if (js.value == 1) {
				switch(js.number) {
					case mute_but :
						mute();
						break;
					case unmute_but :
						unmute();
						break;	
					case freq_up_but :
						center_freq+=freq_step;
						break;
					case freq_dn_but :
						center_freq-=freq_step;
						break;
					case vol_up_but :
						if (lock_vol_up == false) {
							lock_vol_up = true;
							initial_vol+=1;		
							if (initial_vol > 100) {
								initial_vol = 100;
							}			 		 
						}
						break;
					case vol_dn_but :
						if (lock_vol_dn == false) {
							lock_vol_dn = true;
							initial_vol-=1;
							if (initial_vol < 0) {
								initial_vol = 0;
							}			 
						}
						break;				
					case range_up_but :
						if (lock_range_up == false) {
							lock_range_up = true;
							range++;
							if (range > range_max) {
								range = range_max;
							}
						}	
						break;
					case range_dn_but :
						if (lock_range_dn == false) {
							lock_range_dn = true;
							range--;
							if (range < range_min) {
								range = range_min;
							}
						}
						break;
				}
			} 
		}
		usleep(joystick_update_every_us);
	} // while
}

char * getUsage() {
	return "USAGE: sinejoy [-f {starting freqency in Hz}] [-g {starting/min volume 0-100}] [-fa {jostick frequency axis number}] [-va {jostick volume axis number}][-s {sound device i/o path}] [-j {joystick device i/o path] [-b {buffer_size}]  [-v {1|0 for verbose live values default is 1}] [-m {mixer_name - use amixer to see list of entries default is Master}]";
}

int main(int argc, char *argv[]) {
	signal(SIGINT, stopHandler);
  signal(SIGKILL, stopHandler);
  
  //Config cfg;

  // Read the file. If there is an error, report it and exit.
 /* try
  {
    cfg.readFile("alsasinejoy.cfg");
  }
  catch(const FileIOException &fioex)
  {
    std::cerr << "I/O error while reading file." << std::endl;
    return(EXIT_FAILURE);
  }
  catch(const ParseException &pex)
  {
    std::cerr << "Parse error at " << pex.getFile() << ":" << pex.getLine()
              << " - " << pex.getError() << std::endl;
    return(EXIT_FAILURE);
  }*/
  
  /*const Setting& root = cfg.getRoot();
  
  root.lookupvalue("ldc_freq", ldc_freq);
  	
	if (argc % 2 == 1) {
  	int argpairc = argc / 2;
  	int ndx = 0;
  	printf("found settings\n");
  	for (int i=0; i< argpairc; i++) {
  		ndx = i*2+1;
  		if (!strcmp(argv[ndx], "-d") && !strcmp(argv[ndx+1], "dsp")) {
  			use_dsp=true;
  	  } else if (!strcmp(argv[ndx], "-s")) {
  			snd_dev = argv[ndx+1];
  		} else if (!strcmp(argv[ndx], "-m")) {
  			mixer_name = argv[ndx+1];
  		} else if (!strcmp(argv[ndx], "-j")) {
  			joy_dev = argv[ndx+1];
  		} else if (!strcmp(argv[ndx], "-r")) {
  			range = atoi(argv[ndx+1]);
			} else if (!strcmp(argv[ndx], "-b")) {
  			buffer_size = atoi(argv[ndx+1]);
  		} else if (!strcmp(argv[ndx], "-l")) {
  			latency = atoi(argv[ndx+1]);
    	} else if (!strcmp(argv[ndx], "-v")) {
  			disp_live_values = atoi(argv[ndx+1]);
    	} else if (!strcmp(argv[ndx], "-g")) {
  			initial_vol = atoi(argv[ndx+1]);
  			if (initial_vol < 1 || initial_vol > 100) {
  				printf("invalid vol: %i.  Must be > 0 and < 100.\n", initial_vol);
  				return -1;  			
  			}
  		} else if (!strcmp(argv[ndx], "-f")) {
  			center_freq = atoi(argv[ndx+1]);
  			if (center_freq < 1) {
  				printf("invalid freqency: %iHz.  Must be > 0.\n", center_freq);
  				return -1;  			
  			}
  		} else if (!strcmp(argv[ndx], "-fa")) {
  			freq_axis_arg = atoi(argv[ndx+1]);  	
  			if (freq_axis_arg < 0) { 
  				printf("Invalid frequency axis number: %i\n\n%s\n", freq_axis_arg, getUsage());
  				return -1;
  			}
  		} else if (!strcmp(argv[ndx], "-va")) {
  			freq_axis_arg = atoi(argv[ndx+1]);  	
  			if (freq_axis_arg < 0) { 
  				printf("Invalid volume axis number: %i\n\n%s\n", vol_axis_arg, getUsage());
  				return -1;
  			}
  		} else if (!strcmp(argv[ndx], "-ldc") && !strcmp(argv[ndx+1], "off")) {
  			ldc_duty_cycle_pct = 100;
  		} else {
  			printf("Invalid Argument: %s\n\n%s\n", argv[ndx], getUsage());
  			return -1;
  		}  		 		
  	}
  } else if (argc != 1) {
  	printf("%s\n", getUsage());
  	stopHandler();
  	return -1;
  }*/
    
  //printf("finished parsing arguments\n");
  std::thread joythread(joystick_thread);
  
  printf ("duty cycle: %d\n", ldc_duty_cycle_pct);	
  init_ldc_table(ldc_duty_cycle_pct);
	
	const char *axis_heading = "Axis ";
	const char *btn_heading = "Btn ";	
	
	float cur_freq = center_freq;
	float cur_vol = initial_vol;
	
	if (use_dsp) {
		if (strcmp(snd_dev,"default") == 0)
			snd_dev = "/dev/dsp";
		initialize_dsp (snd_dev);
	} else {	
		wave_midpoint = 0;
		initialize_alsa(snd_dev);	
	}
	
	flatline = (unsigned char *) calloc( buffer_size, sizeof( unsigned char ) );
	for (int i=0; i < buffer_size; i++) {
		flatline[i] = wave_midpoint;
	}
	
  while (!joystick_initialized) {} 			
  
	printf ("Initial Freq Axis Value: %d\n", initial_freq_axis_val, freq_axis_arg);
	printf ("push Start on the joystick to begin tone\n\n");
	printHeader();


	int freq_axis_delta = 0;
	int vol_axis_delta = 0;
	int scaler;
	int last_initial_vol = 0;
	
	
	while (keepRunning) 
	{	
		scaler = 32767/range;  				
		freq_axis_delta = freq_axis_val - initial_freq_axis_val;
		if (last_initial_vol != initial_vol) {
			last_initial_vol = initial_vol;
			vol_axis_delta = -1; // force recalculate
			vol_multiplier = calc_vol_multiplier();;
		}
		if (vol_axis_delta != vol_axis_val - initial_vol_axis_val) {			
			vol_axis_delta = vol_axis_val - initial_vol_axis_val;
			cur_vol = (vol_axis_val+32767)*vol_multiplier + initial_vol;
			//setAlsaMasterVolume(cur_vol);
		}
		//printf ("\naD:%i aVi:%i aVo:%i", freq_axis_delta, axis_val, initial_freq_axis_val);
		/* axis values can be negative and you may end up with a -x/range which will
			throw a floating point exception so we'll add the max negative value to 
			the axis delta and then add the precalulated scaler back to get it back */		
		cur_freq = center_freq + (freq_axis_delta + 32767) / range - scaler;
		//cur_freq += 1;
		//printf ("\nfreq: %i\n\n", cur_freq);
		// protect speakers
		if (cur_freq < freq_floor) cur_freq = freq_floor;
		if (cur_freq > freq_ceiling) cur_freq = freq_ceiling;	
		write_wave_from_table(cur_freq,cur_vol);
		if (disp_live_values) {
			printf ("%9d    %9d  %9d    %9d      %9d   %9f   %9f",freq_axis_val,freq_axis_delta,vol_axis_val,vol_axis_delta,center_freq,cur_freq,cur_vol);			
			printf("\r");
			fflush(stdout);
		}
	}
  
  joythread.join();
  return 0;
}

