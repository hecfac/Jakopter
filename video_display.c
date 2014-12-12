#include <SDL2/SDL.h>
#include "video.h"
#include <stdio.h>
#include <pthread.h>


//width and height packed for easy passing.
typedef struct video_size {
	int w, h;
} video_size;
//feedback message the thread can send after an operation
typedef enum feedback_msg {
	FDBK_NONE, FDBK_OK, FDBK_FAIL
} feedback_msg;
/*
* The SDL window where the video is displayed.
*/
static SDL_Window* win = NULL;
/*
* SDL renderer attached to our window
*/
static SDL_Renderer* renderer = NULL;
/*
* Texture that holds the current frame to display. It's the same size as the window.
*/
static SDL_Texture* frameTex = NULL;
/*
* Event types that the display thread will receive.
* frame : we got a new frame. data1 is a pointer to it.
*	data2 is a pointer to a video_size struct containing its size.
* quit : the display thread has been externally requested to end.
*/
static Uint32 event_frame, event_quit;
/*
* Current size of the window and the frame texture.
* Mainly used to check whether it's changed.
*/
static int current_width, current_height;
/*
* Check whether or not the display has been initialized.
*/
static int initialized = 0;
/*
* Handle to the display thread.
*/
static pthread_t thread_handle;
static pthread_mutex_t feedback_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t feedback_cond = PTHREAD_COND_INITIALIZER;
feedback_msg display_fdbk = FDBK_NONE;


/*
* Initialize SDL, create the window and the renderer
* to get ready to draw frames.
* @param w
* @param h width and height with which to create the window.
* @return 0 on success, -1 on error.
*/
static int video_display_init(int width, int height) {
	
	if(SDL_Init(SDL_INIT_VIDEO) < 0) {
		fprintf(stderr, "Display : error initializing SDL : %s\n", SDL_GetError());
		return -1;
	}
	//create a window of the given size, without options. Make it centered.
	win = SDL_CreateWindow("Drone video",
		SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,width,height,0);
	if(win == NULL) {
		fprintf(stderr, "Display : error creating window : %s\n", SDL_GetError());
		return -1;
	}
	
	renderer = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
	if(renderer == NULL) {
		fprintf(stderr, "Display : error creating renderer : %s\n", SDL_GetError());
		return -1;
	}
	
	//register our 2 events (needed to differenciate them from standard SDL events)
	event_frame = SDL_RegisterEvents(2);
	if(event_frame == (Uint32)-1) {
		fprintf(stderr, "Display : error : couldn't initialize events\n");
		return -1;
	}
	event_quit = event_frame+1;
	
	return 0;
}

/*
* Set the size of the window and of the texture containing the video.
* The parameters become the current size.
*/
static int video_display_set_size(int w, int h) {
	SDL_SetWindowSize(win, w, h);
	//re-create the texture, with the new size
	SDL_DestroyTexture(frameTex);
	frameTex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, w, h);
	if(frameTex == NULL) {
		fprintf(stderr, "Display : failed to create frame texture : %s\n", SDL_GetError());
		return -1;
	}
	current_width = w;
	current_height = h;
	return 0;
}

/*
* Clean the display context : close the window and clean SDL structures.
*/
static void video_display_clean() {

	SDL_DestroyTexture(frameTex);
	SDL_DestroyRenderer(renderer);
	SDL_DestroyWindow(win);
	SDL_Quit();
}

/*
* Thread responsible for managing the SDL window,
* including initializing it, drawing the frames
* and handling the events (make the video thread stop when the window is closed.)
* All of this has to be done on the same thread (that's how SDL works).
* NOTE : there might be problems, like input not working on OS X.
*	The solution would be to use a separate process for SDL...
*/
static void* video_display_thread(void* nothing) {
	
	//boolean set by the thread itself to check whether it should stop.
	//to stop the thread from the outside, push an "event_quit".
	int stopped = 0;
	//hold the current event
	SDL_Event current_evt;
	//initialize everything we need to get the display running
	pthread_mutex_lock(&feedback_mutex);
		if(video_display_init(current_width, current_height) < 0) {
			fprintf(stderr, "Display : Failed initialization.\n");
			stopped = 1;
			display_fdbk = FDBK_FAIL;
		}
		else {
			if(video_display_set_size(current_width, current_height) < 0) {
				stopped = 1;
				display_fdbk = FDBK_FAIL;
			}
			else {
				initialized = 1;
				display_fdbk = FDBK_OK;
			}
		}
		pthread_cond_signal(&feedback_cond);
	pthread_mutex_unlock(&feedback_mutex);
	
	//main loop : check for events (new frame, exit request, user input)
	while(!stopped) {
		if(!SDL_WaitEvent(&current_evt)) {
			fprintf(stderr, "Display : error waiting for events : %s\n", SDL_GetError());
			stopped = 1;
		}
		
		if(current_evt.type == event_frame) {
			//display the new frame, send feedback on whether or not it worked.
			pthread_mutex_lock(&feedback_mutex);
			//change the window's size if needed
			video_size* newsize = (video_size*)current_evt.user.data2;
			if(newsize->w != current_width || newsize->h != current_height)
				if(video_display_set_size(newsize->w, newsize->h) < 0) {
					stopped = 1;
					display_fdbk = FDBK_FAIL;
				}
			
			//update the texture with our new frame. There might've been an error earlier, check.
			if(!stopped) {
				if(SDL_UpdateTexture(frameTex, NULL, current_evt.user.data1, current_width) < 0) {
					fprintf(stderr, "Display : failed to update frame texture : %s\n", SDL_GetError());
					stopped = 1;
					display_fdbk = FDBK_FAIL;
				}
				else
					display_fdbk = FDBK_OK;
			}
			pthread_cond_signal(&feedback_cond);
			pthread_mutex_unlock(&feedback_mutex);
		}
		else if(current_evt.type == event_quit)
			stopped = 1;
		//triggered if the user closes the window -> end the video thread
		else if(current_evt.type == SDL_QUIT) {
			video_set_stopped();
			stopped = 1;
		}
		
		//update the window
		SDL_RenderClear(renderer);
		SDL_RenderCopy(renderer, frameTex, NULL, NULL);
		SDL_RenderPresent(renderer);
	}
	//end of the display thread.
	video_display_clean();
	initialized = 0;
	pthread_exit(NULL);
}

/*
* Push an "event_quit" to stop the display thread, if it exists.
*/
static void video_display_stop_thread() {
	SDL_Event quitevent;
	//don't do anything if the thread isn't running
	if(!initialized)
		return;
		
	SDL_zero(quitevent);
	quitevent.type = event_quit;
	SDL_PushEvent(&quitevent);
}

/*
* Create the display thread as a detached thread.
*/
static int video_display_create_thread() {

	//this is a new thread, so no feedback for now.
	display_fdbk = FDBK_NONE;
	pthread_attr_t thread_attribs;
	pthread_attr_init(&thread_attribs);
	pthread_attr_setdetachstate(&thread_attribs, PTHREAD_CREATE_DETACHED);
	
	if(pthread_create(&thread_handle, &thread_attribs, video_display_thread, NULL) < 0) {
		perror("Error creating the display thread");
		return -1;
	}
	
	//the thread has to tell whether the initialization went smoothly
	int ret_value;
	pthread_mutex_lock(&feedback_mutex);
		while(display_fdbk == FDBK_NONE)
			pthread_cond_wait(&feedback_cond, &feedback_mutex);
		ret_value = (display_fdbk == FDBK_OK) ? 0 : -1;
	pthread_mutex_unlock(&feedback_mutex);
	
	return ret_value;
}

/*
* "Got frame" callback.
* Sends the frame to the display thread via an event_frame.
*/
int video_display_frame(uint8_t* frame, int width, int height, int size) {

	//if we get a NULL frame, stop displaying.
	if(frame == NULL) {
		video_display_stop_thread();
		return 0;
	}

	//first time called ? Create the thread.
	if(!initialized) {
		current_width = width;
		current_height = height;
		if(video_display_create_thread() < 0)
			return -1;
	}
	
	//send the frame to the thread
	display_fdbk = FDBK_NONE;
	SDL_Event framevt;
	SDL_zero(framevt);
	framevt.type = event_frame;
	framevt.user.data1 = frame;
	framevt.user.data2 = &(video_size){width, height};
	SDL_PushEvent(&framevt);
	
	//check whether everything went well on the display side.
	//also, this keeps the memory (frame) warm for our thread.
	//TODO: avoid possible deadlock when the thread has been stopped by an SDL_QUIT event...
	int ret_value;
	pthread_mutex_lock(&feedback_mutex);
		while(display_fdbk == FDBK_NONE)
			pthread_cond_wait(&feedback_cond, &feedback_mutex);
		ret_value = (display_fdbk == FDBK_OK) ? 0 : -1;
	pthread_mutex_unlock(&feedback_mutex);
	
	return ret_value;
}
