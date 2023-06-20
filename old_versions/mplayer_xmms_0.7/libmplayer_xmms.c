/*

    libmplayer_xmms.c

    Copyright (c) 2004 Cesar Hernandez <chernandezba@hotmail.com>

    This file is part of mplayer-xmms

    mplayer-xmms is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA


*/

#include <stdio.h>
#include <xmms/plugin.h>

#include <string.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>

#include <gtk/gtk.h>
#include <time.h>

#include <stdlib.h>
#include <pthread.h>

#include <magic.h>

//#include <gthread.h>


//#define DEBUG

#define VERSION "0.8"
#define FECHA "31/10/2004"
#define TEMPFILE "/tmp/mplayer_xmms.pcm"

#define TAMANYO_BUFFER 11025*2*2 /*44100*2*2 */
//#define TAMANYO_BUFFER 5000*2*2 /*44100*2*2 */

InputPlugin plugin_mplayer;


//

int pausado;
int leidos; //bytes leidos del archivo temporal
int detener; //orden de parar la reproduccion
char *title;
struct stat estado_archivo;
FILE *file;
void *buf;
time_t tiempo_inicial;
int entrada_mplayer;
int tuberias[2];
pid_t pid_hijo=0;
int reproduciendo=0;
int notificado_final=1;
char *archivo;
pthread_t decode_thread;
static GtkWidget *about_window = NULL;
char about_window_text[] = "Mplayer_XMMS Input Plugin "VERSION"\nby Cesar Hernandez ("FECHA")\n"
                           "<chernandezba@hotmail.com>\n\n"
													 "Extensiones soportadas:\n\n";

char *extensiones[]={
	".wmv",".wma",".avi",
	".mpg",".mpeg",".mp4",".mov",".qt",".vivo",".fli",".rm",".roq",".dat",".vob",
	".asf",".asx",".part",".pnew",".pbq",
	//".mp3",".ogg",".wav",
	NULL
};


void parar(void);
void retorna_info_cancion(char *filename,char *titulo);

void borra_archivo_temporal(void)
{

	file=fopen(TEMPFILE,"w");
	fclose (file);

}


void tratamiento_senyales (int s) {

	int status;
	pid_t pid;

	struct stat estado_archivo2;


	switch (s) {
		case SIGCHLD:
#ifdef DEBUG
			printf ("libmplayer_xmms: Recibida senyal SIGCHLD\n");
#endif
			pid=waitpid(-1,&status,WNOHANG);
#ifdef DEBUG
			printf ("libmplayer_xmms: waitpid : pid=%d\n",pid);
#endif
			if (pid>0) {
				if (pid==pid_hijo) {
#ifdef DEBUG
					printf ("libmplayer_xmms: Mplayer finalizado\n");
#endif
					pid_hijo=0;

					if (!stat(TEMPFILE,&estado_archivo2)) {
						plugin_mplayer.set_info(title, (estado_archivo.st_size*10)/441/4 ,
														44100*32, 44100,2);
					}
					//parar();
				}
			}
		break;
		default:
		break;
	}
}



void registrar_senyales(void)
{
	if (signal(SIGCHLD,tratamiento_senyales)==SIG_ERR) {
	  printf ("libmplayer_xmms: Error al registrar senyal SIGCHLD\n");
	}
}

int detecta(char *filename)
{

	char *ext;
	int i;
	char *e;

#ifdef DEBUG
	//printf ("libmplayer_xmms: detecta : %s\n",filename);
#endif
	ext=strrchr(filename,'.');
	if(ext)	{
		for (i=0;;i++) {
			e=extensiones[i];
#ifdef DEBUG
			//printf ("libmplayer_xmms: %s\n",e);
#endif
			if (e==NULL) return 0;
			if(!strcasecmp(ext,e))	{
#ifdef DEBUG
				//printf ("libmplayer_xmms: OK : %s\n",filename);
#endif
				return 1;
			}
		}
	}
	return 0;

}

int leer_archivo(void)
{
	int count,i;
	int buffer_cero;
	char *p;

	buffer_cero=0;

	/*
	Aqui debemos intentar llenar el buffer de lectura. Pueden pasar varias cosas:
	-Que no haya audio
	-Que se haya llegado al final del archivo
	-Que mplayer todavia no haya escrito
	*/

	do {
		if (stat(TEMPFILE,&estado_archivo)) return 0;
		if (estado_archivo.st_size<=TAMANYO_BUFFER+leidos) {
#ifdef DEBUG
			printf("libmplayer_xmms: Esperando a llenar el buffer\n");
#endif
			/*xmms_*/usleep(100000);
		}
		else break;
		buffer_cero++;
		if (buffer_cero==200) {
#ifdef DEBUG
			printf("libmplayer_xmms: 200 reintentos esperando a llenar el buffer. No audio?\n");
#endif
			break;
			//detener=1;
			//return 0; //10 reintentos
		}
	} while (pid_hijo); //si mplayer ha acabado, es inutil esperar que llene el buffer!

	count=fread(buf,1,TAMANYO_BUFFER,file);
	if (count<TAMANYO_BUFFER) {
		//Rellenar el resto
		p=&buf[count];
		for (i=TAMANYO_BUFFER-count;i;i--,p++) *p=0;
	}
	if (count<TAMANYO_BUFFER && feof(file)) {
#ifdef DEBUG
		printf("libmplayer_xmms: Final del archivo\n");
#endif
		detener=1;

		//plugin_mplayer.set_info(title, (leidos/441/4) * 10 ,44100*32, 44100,2);

	}
	leidos +=count;
#ifdef DEBUG
	printf("libmplayer_xmms: Leidos %d bytes\n",count);
#endif
	return count;

}


void salir(void)
{
	parar();
}

void *play_loop(void *arg)
{
	//pid_t pid;
	//int status;
	int count;
	int visualizados;
	int kk=0;

	file=fopen(TEMPFILE,"r");

#ifdef DEBUG
	printf("libmplayer_xmms: Abierto archivo, reproduciendo\n");

#endif

	do {

	//Leer un trozo del archivo
		count=leer_archivo();

		if (count) {
			//plugin_mplayer.output->write_audio(buf, count);
			plugin_mplayer.output->write_audio(buf, TAMANYO_BUFFER);
			/*#define INC TAMANYO_BUFFER
			for (visualizados=0;visualizados<count;visualizados+=INC)
			plugin_mplayer.add_vis_pcm(kk+=INC,FMT_S16_LE,2,INC,buf+visualizados);
		//	plugin_mplayer.add_vis_pcm(plugin_mplayer.output->written_time(),FMT_S16_LE,
              //                       2,count,buf);
			*/

#ifdef DEBUG
	printf ("\nlibmplayer_xmms: Ejecutado write_audio\n");
#endif


      while (plugin_mplayer.output->buffer_free() < TAMANYO_BUFFER) {
#ifdef DEBUG
	printf ("\nlibmplayer_xmms: Bucle usleep\n");
#endif

      	/*xmms_*///usleep((TAMANYO_BUFFER/44/2/2)*200);
		   usleep((TAMANYO_BUFFER/44/2/2)*250);
			//debemos esperar el tiempo equivalente a la duracion del buffer /4
			/* Parece que xmms_usleep a veces se queda bloqueada */
		}

#ifdef DEBUG
	printf ("\nlibmplayer_xmms: Despues de usleep\n");
#endif


//			while(plugin_mplayer.output->buffer_playing()) {
			//parece ser que buffer_playing no siempre va bien
			//	usleep((TAMANYO_BUFFER/44/2/2)*(1000/1.1));
			//	usleep((TAMANYO_BUFFER/44/2/2)*(100/1.1));
												//Esperar una 1.1 parte del tiempo
				/*plugin_mplayer.add_vis_pcm(plugin_mplayer.output->written_time(),FMT_S16_LE,
                                     2,count,buf+kk);
												 kk+=512*4;*/
#ifdef DEBUG
//	printf ("\nlibmplayer_xmms: Esperando...\n");
#endif


//			}
		}

	}	while (!detener);
	fclose(file);
#ifdef DEBUG
	printf ("\nlibmplayer_xmms: Cerrado archivo temporal\n");
#endif


	//xmms_usleep (10000);
	while (pid_hijo>0);
#ifdef DEBUG
	printf ("\nlibmplayer_xmms: pthread_exit\n");
#endif
	parar();
	pthread_exit(NULL);

}

void reproducir(char *filename)
{

	int i;
	char buf_title[1024];

	pausado=0;

	if(plugin_mplayer.output->open_audio(FMT_S16_LE,44100,2)==0)
	{
		//free(wav_file);
		//wav_file=NULL;
		return;
	}

	detener=0;

	if ((buf=malloc(TAMANYO_BUFFER))==NULL) return;

	borra_archivo_temporal();

	/*for (i=strlen(filename);i>=0;i--) {
		if (filename[i]=='/') break;
	}

	title=g_strdup(&filename[i+1]);*/
	retorna_info_cancion(filename,buf_title);
	title=g_strdup(buf_title);

	plugin_mplayer.set_info(title, -1/*5000*/ ,44100*32, 44100,2);


	time(&tiempo_inicial);
	leidos=0;


#ifdef DEBUG
	printf ("libmplayer_xmms: Ejecutando mplayer archivo : %s\n",filename);
#endif
	if (pipe(tuberias)<0) {
		printf ("libmplayer_xmms: Error al hacer pipe\n");
		return;
	}

	pid_hijo=fork();
	if (pid_hijo==-1) {
		printf ("libmplayer_xmms: Error al hacer fork\n");
		return;
	}
	if (pid_hijo) {
		entrada_mplayer=dup(tuberias[1]);
		close(tuberias[0]);
		close(tuberias[1]);
		reproduciendo=1;
		notificado_final=0;

		//xmms_usleep(1000000); //Esperar un tiempo razonable, a que mplayer llene el archivo
		pthread_create(&decode_thread, NULL, play_loop, NULL);

		return;
	}
	else {
		close(0);
		dup(tuberias[0]);
		close(tuberias[1]);
		close(tuberias[0]);

		execl("/usr/bin/mplayer","/usr/bin/mplayer",
		/*"-vo","sdl",*/"-framedrop","-slave","-ao","pcm","-nowaveheader",
		"-channels","2","-srate","44100","-autosync","9999","-delay","2",
		"-aofile",TEMPFILE,filename,NULL);


	//mplayer archivo -ao pcm -nowaveheader -aofile /tmp/mplayer_xmms.pcm
		perror ("libmplayer_xmms: Error al ejecutar mplayer : ");
	}

}


void pausa(short paused)
{
	if (pid_hijo>0) write (entrada_mplayer,"pause\n",strlen("pause\n"));
	plugin_mplayer.output->pause(paused);
#ifdef DEBUG
	printf ("libmplayer_xmms: pausa= %d\n",paused);
#endif
	pausado=paused;

}

int da_tiempo (void)
{

  if(reproduciendo)
		return plugin_mplayer.output->output_time();

 return -1;

/*
	time_t tiempo_ahora;

	if (!reproduciendo && !notificado_final) {
		notificado_final=1;
		return -1;
	}

	time(&tiempo_ahora);

	return difftime(tiempo_ahora,tiempo_inicial);
	//return 600;*/
}

void posicionar_lectura(int segundos)
{

	leidos=segundos*44100*4;
	fseek(file,leidos,SEEK_SET);
}

void busqueda (int time)
{
	char buffer[80];
	/*if (pid_hijo>0) {
		sprintf (buffer,"seek %d\n",time);
		write (entrada_mplayer,buffer,strlen(buffer));
	}*/
	if (!pid_hijo) {
#ifdef DEBUG
		printf ("libmplayer_xmms: seek = %d\n",time); //en segundos
#endif
		posicionar_lectura(time);
		plugin_mplayer.output->flush(time*1000);

	}
}

void inicio(void)
{
	registrar_senyales();
}

void about (void)
{


	GtkWidget *dialog_vbox1;
	GtkWidget *hbox1;
	GtkWidget *label1;
	GtkWidget *dialog_action_area1;
	GtkWidget *about_exit;
	char *p = NULL, *q;
	int i,e;

	if (!about_window)
	{
		about_window = gtk_dialog_new();
		gtk_object_set_data(GTK_OBJECT(about_window), "about_window", about_window);
		gtk_window_set_title(GTK_WINDOW(about_window), "Acerca libmplayer_xmms "VERSION);
		gtk_window_set_policy(GTK_WINDOW(about_window), FALSE, FALSE, FALSE);
		gtk_window_set_position(GTK_WINDOW(about_window), GTK_WIN_POS_MOUSE);
		gtk_signal_connect(GTK_OBJECT(about_window), "destroy", GTK_SIGNAL_FUNC(gtk_widget_destroyed), &about_window);
		gtk_container_border_width(GTK_CONTAINER(about_window), 10);

		dialog_vbox1 = GTK_DIALOG(about_window)->vbox;
		gtk_object_set_data(GTK_OBJECT(about_window), "dialog_vbox1", dialog_vbox1);
		gtk_widget_show(dialog_vbox1);
		gtk_container_border_width(GTK_CONTAINER(dialog_vbox1), 5);

		hbox1 = gtk_hbox_new(FALSE, 0);
		gtk_object_set_data(GTK_OBJECT(about_window), "hbox1", hbox1);
		gtk_widget_show(hbox1);
		gtk_box_pack_start(GTK_BOX(dialog_vbox1), hbox1, TRUE, TRUE, 0);
		gtk_container_border_width(GTK_CONTAINER(hbox1), 5);
		gtk_widget_realize(about_window);

		for (i=0,e=0;extensiones[i]!=NULL; i++)
		{
			if (!p)
			{
				p = strdup(extensiones[i]);
			}
			else {
				q = malloc(strlen(p)+strlen(extensiones[i])+3);
				strcpy(q,p);
				e++;
				if (e==4) {
					e=0;
					if (extensiones[i]!=NULL) strcat(q,",\n");
				}
				else {
					if (extensiones[i]!=NULL)
						strcat(q,", ");
				}
				strcat(q,extensiones[i]);
				p=q;
			}
		}
		q =malloc(strlen(p)+strlen(about_window_text)+1);
		strcpy(q,about_window_text);
		strcat(q,p);
		p = q;
		q=NULL;

		label1 = gtk_label_new(p);
		gtk_object_set_data(GTK_OBJECT(about_window), "label1", label1);
		gtk_widget_show(label1);
		gtk_box_pack_start(GTK_BOX(hbox1), label1, TRUE, TRUE, 0);

		dialog_action_area1 = GTK_DIALOG(about_window)->action_area;
		gtk_object_set_data(GTK_OBJECT(about_window), "dialog_action_area1", dialog_action_area1);
		gtk_widget_show(dialog_action_area1);
		gtk_container_border_width(GTK_CONTAINER(dialog_action_area1), 10);

		about_exit = gtk_button_new_with_label("Ok");
		gtk_signal_connect_object(GTK_OBJECT(about_exit), "clicked",
			GTK_SIGNAL_FUNC(gtk_widget_destroy),
			GTK_OBJECT(about_window));

		gtk_object_set_data(GTK_OBJECT(about_window), "about_exit", about_exit);
		gtk_widget_show(about_exit);
		gtk_box_pack_start(GTK_BOX(dialog_action_area1), about_exit, TRUE, TRUE, 0);



		gtk_widget_show(about_window);
	}
	else
	{
		gdk_window_raise(about_window->window);
	}
}

void retorna_info_cancion(char *filename,char *titulo)
{
	magic_t file_cookie;
	char *descripcion=NULL;
	
	int i;
	
	for (i=strlen(filename);i>=0;i--) {
		if (filename[i]=='/') break;
	}
	
	
	file_cookie=magic_open(MAGIC_NONE);
	if (file_cookie!=NULL) {
		if (magic_load(file_cookie,NULL)!=-1) {
			descripcion=magic_file(file_cookie,filename);
			if (descripcion!=NULL) {
				
				//printf ("mplayer_xmms: Descripcion: %s\n",descripcion);
			}
		}
	}
	
	sprintf(titulo,"%s [%s]",&filename[i+1],( descripcion!=NULL ? descripcion : "desconocido"));

}

void info_cancion (char *filename, char **title, int *length)
{
	// Function to grab the title string

	char titulo[1024];
	
	*length = -1; //1000;
	retorna_info_cancion(filename,titulo);

	*title=g_strdup(titulo);


}


InputPlugin plugin_mplayer=
{
	0, //void *handle;
	0, //char *filename;		//* Filled in by xmms
	"mplayer_xmms plugin "VERSION,	//* The description that is shown in the preferences box
	inicio,  //void (*init) (void);	//* Called when the plugin is loaded
	about,  //void (*about) (void);	//* Show the about box
	0,  //void (*configure) (void);
	detecta, //int (*is_our_file) (char *filename);	//* Return 1 if the plugin can handle the file
	0, //GList *(*scan_dir) (char *dirname);	//* Look in Input/cdaudio/cdaudio.c to see how
	//* to use this
	reproducir, //void (*play_file) (char *filename);	//* Guess what...
	parar, //void (*stop) (void);	//* Tricky one
	pausa, //void (*pause) (short paused);//* Pause or unpause
	busqueda, //void (*seek) (int time);	//* Seek to the specified time
	0, //void (*set_eq) (int on, float preamp, float *bands);	//* Set the equalizer, most plugins won't be able to do this
	da_tiempo, //int (*get_time) (void);	//* Get the time, usually returns the output plugins output time
	0, //void (*get_volume) (int *l, int *r);	//* Input-plugin specific volume functions, just provide a NULL if
	0, //void (*set_volume) (int l, int r);	//*  you want the output plugin to handle it
	salir, //void (*cleanup) (void);			//* Called when xmms exit
	0, //InputVisType (*get_vis_type) (void); //* OBSOLETE, DO NOT USE!
	0, //void (*add_vis_pcm) (int time, AFormat fmt, int nch, int length, void *ptr); //* Send data to the visualization plugins
											//Preferably 512 samples/block
	0, //void (*set_info) (char *title, int length, int rate, int freq, int nch);	//* Fill in the stuff that is shown in the player window
											 //  set length to -1 if it's unknown. Filled in by xmms
	0, //void (*set_info_text) (char *text);	//* Show some text in the song title box in the main window,
						   //call it with NULL as argument to reset it to the song title.
						   //Filled in by xmms
	info_cancion, //void (*get_song_info) (char *filename, char **title, int *length);	//* Function to grab the title string
	0, //void (*file_info_box) (char *filename);		//* Bring up an info window for the filename passed in
	0 //OutputPlugin *output;	//* Handle to the current output plugin. Filled in by xmms
};


InputPlugin *get_iplugin_info(void)
{
	return &plugin_mplayer;
}

void parar(void)
{
	int v;

#ifdef DEBUG
		printf ("libmplayer_xmms: parar()\n");
#endif

	if (pid_hijo>0) {
#ifdef DEBUG
		printf ("libmplayer_xmms: Cerrando mplayer pid=%d \n",pid_hijo);
#endif
		v=kill(pid_hijo,SIGTERM); //SIGINT
		pid_hijo=0;
		if (v) perror("libmplayer_xmms: Error al parar mplayer : ");
	}



	if (reproduciendo) {
#ifdef DEBUG
		printf ("libmplayer_xmms: parar reproduccion\n");
#endif
		if (pausado) pausa(0);
		detener=1;
		reproduciendo=0;
		pthread_join(decode_thread,NULL);
#ifdef DEBUG
		printf ("libmplayer_xmms: retornado de pthread_join\n");
#endif
		plugin_mplayer.output->close_audio();
		free(buf);

		borra_archivo_temporal();

	}



}
