/* this is the linux 2.2.x way of handling joysticks. It allows an arbitrary
 * number of axis and buttons. It's event driven, and has full signed int
 * ranges of the axis (-32768 to 32767). It also lets you pull the joysticks
 * name. The only place this works of that I know of is in the linux 1.x 
 * joystick driver, which is included in the linux 2.2.x kernels
 */

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/joystick.h>


int main(int argc, char *argv[])
{

  char *joy_dev = "/dev/input/js0"; 
  
  if (argc > 1)
  	joy_dev = argv[1];

	int joy_fd, *axis=NULL, num_of_axis=0, num_of_buttons=0, x, i;
	char *button=NULL, name_of_joystick[80];
	struct js_event js;

	if( ( joy_fd = open( joy_dev , O_RDONLY)) == -1 )
	{
		printf( "Couldn't open joystick at %s\n", joy_dev );
		return -1;
	}

	ioctl( joy_fd, JSIOCGAXES, &num_of_axis );
	ioctl( joy_fd, JSIOCGBUTTONS, &num_of_buttons );
	ioctl( joy_fd, JSIOCGNAME(80), &name_of_joystick );

	axis = (int *) calloc( num_of_axis, sizeof( int ) );
	button = (char *) calloc( num_of_buttons, sizeof( char ) );
	
	printf("Joystick detected: %s\n\t%d axis\n\t%d buttons\n\n"
		, name_of_joystick
		, num_of_axis
		, num_of_buttons );

	fcntl( joy_fd, F_SETFL, O_NONBLOCK );	/* use non-blocking mode */

	char *axis_heading = "Axis ";
	char *btn_heading = "Btn ";
	
	
	for (i=0; i<num_of_axis; ++i)
		printf ("Axis %i|", i);
	for( x=0 ; x<num_of_buttons ; ++x )
		printf("Btn %i|", x);
	printf("\n");
	fflush(stdout);

	while( 1 ) 	/* infinite loop */
	{

			/* read the joystick state */
		read(joy_fd, &js, sizeof(struct js_event));
		
			/* see what to do with the event */
		switch (js.type & ~JS_EVENT_INIT)
		{
			case JS_EVENT_AXIS:
				axis   [ js.number ] = js.value;
				break;
			case JS_EVENT_BUTTON:
				button [ js.number ] = js.value;
				break;
		}

		for (i=0; i<num_of_axis; ++i)
			printf ("%6d|",axis[i]);
		for( x=0 ; x<num_of_buttons ; ++x ) {
			if (x > 8) {
				printf("%6d|",button[x]);
			} else {
				printf("%5d|",button[x]);
			}
		}
		printf("\r");
		fflush(stdout);
	}

	close( joy_fd );	/* too bad we never get here */
	return 0;
}
