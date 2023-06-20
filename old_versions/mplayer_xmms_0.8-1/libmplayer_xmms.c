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
#include <xmms/configfile.h>

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


//#define DEBUG

#define VERSION "0.8-1"
#define FECHA "06/01/2006"
#define TEMPFILE "/tmp/mplayer_xmms.pcm"

//con archivo temporal: #define TAMANYO_BUFFER 11025*2*2 /*44100*2*2 */
//#define TAMANYO_BUFFER 11025*2*2 /*44100*2*2 */

//el tamaño de una pipe es 4096 bytes
//#define TAMANYO_BUFFER 4096

#define TAMANYO_BUFFER_PIPE 4096
#define TAMANYO_BUFFER_FILE 4096 //11000 //11025*2*2



InputPlugin plugin_mplayer;

int tamanyo_buffer;
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
static GtkWidget *configure_window = NULL;
static GtkWidget *file_info_window = NULL;
GtkWidget *checkbutton_pipe;

char about_window_text[] = "Mplayer_XMMS Input Plugin "VERSION"\nby Cesar Hernandez ("FECHA")\n"
                           "<chernandezba@hotmail.com>\n\n"
													 "Supported Extensions:\n\n";

char *extensiones[]={
	".wmv",".wma",".avi",
	".mpg",".mpeg",".mp4",".mov",".qt",".vivo",".fli",".rm",".roq",".dat",".vob",
	".asf",".asx",".part",".pnew",".pbq",
	//".mp3",".ogg",".wav",
	NULL
};


struct mplayerxmmsconfig {
	int cfg_use_pipe; //valor actual de use_pipe
	int use_pipe; //valor actual de use_pipe para la reproduccion actual
													
};
struct mplayerxmmsconfig mplayerxmms_config={TRUE,TRUE};
//el comportamiento por defecto será usar pipe, pese que a esto no se define aqui, sino en la funcion
//lee_configuracion


void parar(void);
void retorna_info_cancion(char *filename,char *titulo);
void activa_cambios_configuracion(void);

void borra_archivo_temporal(void)
{

	unlink (TEMPFILE);

}

void crea_archivo_temporal(void)
{

	int retorno;

	if (mplayerxmms_config.use_pipe==FALSE) {

		file=fopen(TEMPFILE,"w");
		fclose (file);
		chmod(TEMPFILE,S_IRUSR | S_IWUSR );

	}
	
	else {
		retorno=mkfifo(TEMPFILE,0x180); //0600 octal 
		
		#ifdef DEBUG
		printf ("libmplayer_xmms: retorno mkfifo: %d\n",retorno);
		if (retorno==-1) {
			perror ("libmplayer_xmms: error mkfifo : ");
		}
	#endif

	}
	

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

	//Mirar de llenar el buffer. No tiene sentido con pipe.
	//Lo ideal seria hacer pipe con streams de video, y archivo con streams de audio

	if (mplayerxmms_config.use_pipe==FALSE) {
			
		do {
			if (stat(TEMPFILE,&estado_archivo)) return 0;
			if (estado_archivo.st_size<=tamanyo_buffer+leidos) {
	#ifdef DEBUG
				printf("libmplayer_xmms: Esperando a llenar el buffer\n");
	#endif
				usleep(100000);
			}
			else break;
			buffer_cero++;
			if (buffer_cero==200) {
	#ifdef DEBUG
				printf("libmplayer_xmms: 200 reintentos esperando a llenar el buffer. No audio?\n");
	#endif
				break;

			}
		} while (pid_hijo); //si mplayer ha acabado, es inutil esperar que llene el buffer!
	
	}

	count=fread(buf,1,tamanyo_buffer,file);
	if (count<tamanyo_buffer) {
		//Rellenar el resto
		p=&((char *)buf)[count];
		for (i=tamanyo_buffer-count;i;i--,p++) *p=0;
	}
	if (count<tamanyo_buffer && feof(file)) {
#ifdef DEBUG
		printf("libmplayer_xmms: Final del archivo\n");
#endif
		detener=1;

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
	//int visualizados;

	file=fopen(TEMPFILE,"r");

#ifdef DEBUG
	printf("libmplayer_xmms: Abierto archivo, reproduciendo\n");

#endif

#ifdef DEBUG
	printf ("libmplayer_xmms: inicial buffer_free:%d\n",plugin_mplayer.output->buffer_free());
#endif
	
	//Ajustar tamaño buffer si es necesario
	if (plugin_mplayer.output->buffer_free()<tamanyo_buffer) {
		tamanyo_buffer=plugin_mplayer.output->buffer_free();
	}
	
	do {

	//Leer un trozo del archivo
		count=leer_archivo();

		if (count) {
			plugin_mplayer.output->write_audio(buf, tamanyo_buffer);

#ifdef DEBUG
	printf ("\nlibmplayer_xmms: Ejecutado write_audio\n");
#endif


			while (plugin_mplayer.output->buffer_free() < tamanyo_buffer) {
#ifdef DEBUG
			printf ("\nlibmplayer_xmms: Bucle usleep buffer_free=%d tamanyo_buffer:%d\n",
			plugin_mplayer.output->buffer_free(),tamanyo_buffer);
#endif
				//Esperar 1 decima de segundo menos de lo que se tarda en reproducir el buffer
				usleep( (tamanyo_buffer*1000/44/2/2)  - 100);
				
			}

#ifdef DEBUG
	printf ("\nlibmplayer_xmms: Despues de usleep\n");
#endif

		}

	}	while (!detener);
	fclose(file);
#ifdef DEBUG
	printf ("\nlibmplayer_xmms: Cerrado archivo temporal\n");
#endif


	while (pid_hijo>0);
#ifdef DEBUG
	printf ("\nlibmplayer_xmms: pthread_exit\n");
#endif
	parar();
	pthread_exit(NULL);

}

void reproducir(char *filename)
{

	//int i;
	char buf_title[1024];

	pausado=0;

	if(plugin_mplayer.output->open_audio(FMT_S16_LE,44100,2)==0)
	{
		//free(wav_file);
		//wav_file=NULL;
		return;
	}

	detener=0;

	//Poner la configuracion activa
	mplayerxmms_config.use_pipe=mplayerxmms_config.cfg_use_pipe;
	
	if (mplayerxmms_config.use_pipe==TRUE) tamanyo_buffer=TAMANYO_BUFFER_PIPE;
	else tamanyo_buffer=TAMANYO_BUFFER_FILE;
	
	if ((buf=malloc(tamanyo_buffer))==NULL) return;

	crea_archivo_temporal();

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

		pthread_create(&decode_thread, NULL, play_loop, NULL);

		return;
	}
	else {
		close(0);
		dup(tuberias[0]);
		close(tuberias[1]);
		close(tuberias[0]);

		if (mplayerxmms_config.use_pipe==TRUE) {
			execl("/usr/bin/mplayer","/usr/bin/mplayer",
			"-framedrop","-slave","-ao","pcm:nowaveheader:file="TEMPFILE,
			"-channels","2","-srate","44100",
			//prueba
			"-vo","x11",
			//
			filename,NULL);
		}
		else 
		{
			execl("/usr/bin/mplayer","/usr/bin/mplayer",
			"-framedrop","-slave","-ao","pcm:nowaveheader:file="TEMPFILE,
			"-channels","2","-srate","44100",
			//prueba
			"-vo","x11",
			"-autosync","9999","-delay","2",
			filename,NULL);
		}

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
	//En modo pipe aqui no se llega nunca ????
	//es mas, si se llegase, no tendria sentido hacer un fseek en una pipe
	leidos=segundos*44100*4;
	fseek(file,leidos,SEEK_SET);
}

void busqueda (int time)
{
	//char buffer[80];
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


void guarda_configuracion(void)
{

	ConfigFile *cf;

	
	cf = xmms_cfg_open_default_file();
	if (cf == NULL) cf = xmms_cfg_new();
	
	
	//Guardar los valores cambiados desde el dialogo de configuracion
	xmms_cfg_write_boolean(cf,"MPLAYERXMMS","use_pipe",mplayerxmms_config.cfg_use_pipe);
	
	xmms_cfg_write_default_file(cf);
	xmms_cfg_free(cf);

}

void lee_configuracion(void)
{

	ConfigFile *cf;

	
	//inicializar por si no tenemos todavia configuracion guardada
	mplayerxmms_config.cfg_use_pipe=TRUE;
	
	//leer configuracion
	cf = xmms_cfg_open_default_file();
	if (cf != NULL) {
		xmms_cfg_read_boolean(cf, "MPLAYERXMMS", "use_pipe",
								&mplayerxmms_config.cfg_use_pipe);
	
		xmms_cfg_free(cf);
	}
	
	//Guardamos configuracion por primera vez
	
	//como sabemos si hay configuracion para MPLAYERXMMS?
	//si no hay configuracion por defecto, será la indicada al principio de esta funcion
	//se guardara por primera vez al entrar en configuracion

}

void inicio(void)
{

#ifdef DEBUG
	printf ("libmplayer_xmms: inicio()\n");
#endif
	registrar_senyales();
	lee_configuracion();	
	
}


void ok_configure(GtkWidget *w, gpointer data)
{
	mplayerxmms_config.cfg_use_pipe=gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(checkbutton_pipe));
	guarda_configuracion();
	//los valores de configuracion nuevos solo se activaran al ejecutar reproducir(), o sea,
	//al cambiar de cancion, asi no se hará el cambio en caliente
	
	gtk_widget_destroy(configure_window);
}

void cancel_configure(GtkWidget *w, gpointer data)
{
	gtk_widget_destroy(configure_window);
}



void configure (void)
{

/*

Dialogo:

------------------------------------
|                                  |
|  ------------------------------  |
|  |          vbox1             |  |
|  |    --------------------    |  |
|  |    | checkbutton_pipe |    |  |
|  |    |------------------|    |  |
|  |                            |  |
|  |    |-----------------|     |  |
|  |    |      hbox1      |     |  |
|  |    |                 |     |  |
|  |    |  ----  -------- |     |  |
|  |    |  |Ok|  |Cancel| |     |  |
|  |    |  ----  -------- |     |  | 
|  |    |------------------     |  |
|  |                            |  |
|  |-----------------------------  |
|                                  |
------------------------------------


*/


	GtkWidget *dialog_vbox1;
	GtkWidget *dialog_hbox1;
	GtkWidget *configure_ok;
	GtkWidget *configure_cancel;

	if (!configure_window)
	{
	
		configure_window = gtk_dialog_new();
		
		//Definir ventana
		gtk_object_set_data(GTK_OBJECT(configure_window), "about_window", configure_window);
		gtk_window_set_title(GTK_WINDOW(configure_window), "Configure");
		gtk_window_set_policy(GTK_WINDOW(configure_window), FALSE, FALSE, FALSE);
		gtk_window_set_position(GTK_WINDOW(configure_window), GTK_WIN_POS_MOUSE);
		gtk_signal_connect(GTK_OBJECT(configure_window), "destroy", GTK_SIGNAL_FUNC(gtk_widget_destroyed),
								&configure_window);
		gtk_container_border_width(GTK_CONTAINER(configure_window), 10);
		
		//Definir dialogo
		dialog_vbox1 = GTK_DIALOG(configure_window)->vbox;
		//dialog_vbox1 = gtk_vbox_new (FALSE, 0);
		//gtk_container_add (GTK_CONTAINER (configure_window), dialog_vbox1);
		gtk_object_set_data(GTK_OBJECT(configure_window), "dialog_vbox1", dialog_vbox1);
		gtk_widget_show(dialog_vbox1);
		gtk_container_border_width(GTK_CONTAINER(dialog_vbox1), 5);
		
		//Definir checkbutton pipe
		checkbutton_pipe = gtk_check_button_new_with_label ("Use pipe instead\nof temporal file");
		gtk_widget_show (checkbutton_pipe);
		gtk_box_pack_start (GTK_BOX (dialog_vbox1), checkbutton_pipe, FALSE, FALSE, 0);
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(checkbutton_pipe), mplayerxmms_config.cfg_use_pipe);
		
		//Definir dialogo horizontal dentro de dialogo vertical
		dialog_hbox1 = gtk_hbox_new(FALSE, 0);
		gtk_object_set_data(GTK_OBJECT(configure_window), "dialog_hbox1", dialog_hbox1);
		gtk_widget_show(dialog_hbox1);
		gtk_box_pack_start(GTK_BOX(dialog_vbox1), dialog_hbox1, TRUE, TRUE, 0);
		gtk_container_border_width(GTK_CONTAINER(dialog_hbox1), 5);
		
		
		
		//Definir boton ok
		configure_ok = gtk_button_new_with_label("   Ok   ");
		gtk_object_set_data(GTK_OBJECT(configure_window), "configure_ok", configure_ok);
		gtk_widget_show(configure_ok);
		gtk_box_pack_start(GTK_BOX(dialog_hbox1), configure_ok, FALSE, FALSE, 0);
		gtk_signal_connect_object(GTK_OBJECT(configure_ok), "clicked",GTK_SIGNAL_FUNC(ok_configure),NULL);

		//Definir boton cancelar
		configure_cancel = gtk_button_new_with_label(" Cancel ");
		gtk_object_set_data(GTK_OBJECT(configure_window), "configure_exit", configure_cancel);
		gtk_widget_show(configure_cancel);
		gtk_box_pack_start(GTK_BOX(dialog_hbox1), configure_cancel, FALSE, FALSE, 0);
		gtk_signal_connect(GTK_OBJECT(configure_cancel), "clicked", GTK_SIGNAL_FUNC(cancel_configure),NULL);

		
		gtk_widget_realize(configure_window);
		
		gtk_widget_show(configure_window);
	}
	
	else
	{
		gdk_window_raise(configure_window->window);
	}
		

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
		gtk_window_set_title(GTK_WINDOW(about_window), "About ... ");
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

//Retorna la descripcion de la cancion (tipo de datos del stream)
void retorna_descripcion_cancion(char *filename,char *descripcion)
{
	magic_t file_cookie;
	char *info_stream=NULL;
	
	file_cookie=magic_open(MAGIC_NONE);
	if (file_cookie!=NULL) {
		if (magic_load(file_cookie,NULL)!=-1) {
			info_stream=(char *)magic_file(file_cookie,filename);
		}
	}
	
	sprintf(descripcion,"%s",( info_stream!=NULL ? info_stream : "desconocido"));

}

//Retorna el nombre del archivo y la descripcion entre corchetes
void retorna_info_cancion(char *filename,char *titulo)
{

	char descripcion[500];
	
	int i;
	
	for (i=strlen(filename);i>=0;i--) {
		if (filename[i]=='/') break;
	}
	
	retorna_descripcion_cancion(filename,descripcion);
	
	sprintf(titulo,"%s [%s]",&filename[i+1],descripcion);

}

void info_cancion (char *filename, char **title, int *length)
{
	// Function to grab the title string

	char titulo[1024];
	
	*length = -1; //1000;
	retorna_info_cancion(filename,titulo);

	*title=g_strdup(titulo);


}

void ver_info_cancion (char *filename)
{

/* Dialogo:

un vbox de 2 filas dentro de un dialogo.
Cada vbox tiene un hbox de 2 columnas.


*/
	char tipo[1024];
	GtkWidget *vbox1;
	GtkWidget *hbox1;
	GtkWidget *label1;
	GtkWidget *entry3;
	GtkWidget *hbox2;
	GtkWidget *label2;
	GtkWidget *entry2;
	
	GtkTooltips *tooltips;

	
	retorna_descripcion_cancion(filename,tipo);
	
	if (!file_info_window)
	{
	
		file_info_window = gtk_dialog_new();
		gtk_object_set_data(GTK_OBJECT(file_info_window), "file_info_window", file_info_window);
		gtk_window_set_title(GTK_WINDOW(file_info_window), "Info ... ");
		gtk_window_set_policy(GTK_WINDOW(file_info_window), FALSE, FALSE, FALSE);
		gtk_window_set_position(GTK_WINDOW(file_info_window), GTK_WIN_POS_MOUSE);
		gtk_signal_connect(GTK_OBJECT(file_info_window), "destroy", GTK_SIGNAL_FUNC(gtk_widget_destroyed),
									&file_info_window);
		gtk_container_border_width(GTK_CONTAINER(file_info_window), 10);
		
		
		vbox1 = GTK_DIALOG(file_info_window)->vbox;		
		
		//vbox1 = gtk_vbox_new (FALSE, 0);
		gtk_widget_show (vbox1);
		//gtk_container_add (GTK_CONTAINER (window1), vbox1);
		
		hbox1 = gtk_hbox_new (FALSE, 0);
		gtk_widget_show (hbox1);
		gtk_box_pack_start (GTK_BOX (vbox1), hbox1, TRUE, TRUE, 0);
		
		label1 = gtk_label_new ("Archivo: ");
		gtk_widget_show (label1);
		gtk_box_pack_start (GTK_BOX (hbox1), label1, FALSE, FALSE, 0);
		
		entry3 = gtk_entry_new ();
		gtk_widget_show (entry3);
		gtk_box_pack_start (GTK_BOX (hbox1), entry3, TRUE, TRUE, 0);
		gtk_editable_set_editable (GTK_EDITABLE (entry3), FALSE);
		//gtk_set_entry (
		gtk_entry_set_text(GTK_ENTRY (entry3),filename);
				
		hbox2 = gtk_hbox_new (FALSE, 0);
		gtk_widget_show (hbox2);
		gtk_box_pack_start (GTK_BOX (vbox1), hbox2, TRUE, TRUE, 0);
		
		label2 = gtk_label_new ("Tipo de datos: ");
		gtk_widget_show (label2);
		gtk_box_pack_start (GTK_BOX (hbox2), label2, FALSE, FALSE, 0);
		
		
		entry2 = gtk_entry_new ();
		gtk_widget_show (entry2);
		gtk_box_pack_start (GTK_BOX (hbox2), entry2, TRUE, TRUE, 0);
		gtk_editable_set_editable (GTK_EDITABLE (entry2), FALSE);
		gtk_entry_set_text(GTK_ENTRY (entry2),tipo);
		//Mostrar en una sugerencia el tipo de archivo
		tooltips = gtk_tooltips_new ();		
		gtk_tooltips_set_tip (tooltips, entry2, tipo, NULL);

		
		
		gtk_widget_show(file_info_window);
	
	}
	else
	{
		gdk_window_raise(file_info_window->window);
	}	
	
	
	

}


InputPlugin plugin_mplayer=
{
	0, //void *handle;
	0, //char *filename;		//* Filled in by xmms
	"mplayer_xmms plugin "VERSION,	//* The description that is shown in the preferences box
	inicio,  //void (*init) (void);	//* Called when the plugin is loaded
	about,  //void (*about) (void);	//* Show the about box
	configure,  //void (*configure) (void);
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
	ver_info_cancion, //void (*file_info_box) (char *filename);		//* Bring up an info window for the filename passed in
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
