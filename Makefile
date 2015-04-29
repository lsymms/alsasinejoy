all: alsa_example

alsa_example: alsa_example.c
	g++ alsa_example.c -lasound -o alsa_example

i2c8bittest: i2c8bittest.cpp i2c8bit.o 
	g++ i2c8bittest.cpp -o i2c8bittest i2c8bit.o

i2c8bit.o: i2c8bit.cpp i2c8bit.h
	g++ -c i2c8bit.cpp

alsasinejoy: alsasinejoy.cpp
	g++ -std=c++0x alsasinejoy.cpp -lasound -o alsasinejoy INIReader.o ini.o

#alsasinejoy.c: phonytarget
#	wget -r https://dl.dropbox.com/u/46715231/alsasinejoy.c

phonytarget:
	echo "always do this"

func_map: func_map.cpp
	g++ func_map.cpp -o func_map
