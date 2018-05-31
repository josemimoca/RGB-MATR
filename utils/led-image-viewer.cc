// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://gnu.org/licenses/gpl-2.0.txt>

// To use this image viewer, first get image-magick development files
// $ sudo apt-get install libgraphicsmagick++-dev libwebp-dev
//
// Then compile with
// $ make led-image-viewer

#include "led-matrix.h"			// Llamada a bibliotecas necesarias
#include "pixel-mapper.h"
#include "content-streamer.h"

#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include <Magick++.h>
#include <magick/image.h>

using rgb_matrix::GPIO;				// Uso de parametros de las bibliotecas incluidas
using rgb_matrix::Canvas;
using rgb_matrix::FrameCanvas;
using rgb_matrix::RGBMatrix;
using rgb_matrix::StreamReader;

typedef int64_t tmillis_t;	// Declara tmillis_t del tipo int64_t, cada vez que se utilice, se referira a dicho tipo de datos
static const tmillis_t distant_future = (1LL<<40); // Declara una variable estatica de valor 1*2^40 en milisegundos

struct ImageParams {	// Declara una estructura, parametros de tiempo principalmente
  ImageParams() : anim_duration_ms(distant_future), wait_ms(1500),	// Inicializacion de la estructura ImageParams
                  anim_delay_ms(-1), loops(-1) {}	
  tmillis_t anim_duration_ms;  // Duracion del Gif, en caso de introducir uno.
  tmillis_t wait_ms;           // Tiempo por defecto a mostrar una imagen, milisegundos.
  tmillis_t anim_delay_ms;     // Espaciado entre imagenes, milisegundos.
  int loops;				   // Numero de repeticiones que se realizaran del programa, -1 se corresponde a 1 unico bucle 
};

struct FileInfo {		// Estructura que define el tipo de archivo que maneja
  ImageParams params;      // Declara una variable de tipo estructura (ImageParams)
  bool is_multi_frame;	   // Variable booleana que contendra la informacion de si el archivo se trata de una animacion o una imagen
  rgb_matrix::StreamIO *content_stream;	// Declara una variable (content_stream), del tipo clase (StreamIO) biblioteca content-streamer.h
};

volatile bool interrupt_received = false;	// Declara una variable volatil global que rige la interrupcion de la funcion main
static void InterruptHandler(int signo) {	// Funcion estatica que controla el valor de la variable interrupt_received
  interrupt_received = true;				// Pone a true la variable que detendra la funcion main	
}

static tmillis_t GetTimeInMillis() {	// Funcion estatica que obtiene la fecha en ms desde un origen establecido, devuelve un tiempo en ms
  struct timeval tp;	// Estructura propia de <sys/time.h>, que obtiene la cantidad de ms que han pasado desde el 1 de enero de 1970
  gettimeofday(&tp, NULL);	// &tp se refiere a el como puntero, NULL es necesario para el tipo de resultado que se busca que genere gettimeofday
  return tp.tv_sec * 1000 + tp.tv_usec / 1000;	// Precision de la funcion hasta us, devuelve un tiempo en ms. Dichos valores van a la estructura tp
}

static void SleepMillis(tmillis_t milli_seconds) {	// Toma como entrada una variable (milli_seconds), de tipo tmillis_t, obtenido previamente
													// Devuelve el tiempo en ms que estara la funcion main en parada
  if (milli_seconds <= 0) return;	// Si a variable temporal es negativa, sale de la funcion
  struct timespec ts;	// Define la structura ts del tipo timespec, propia de <sys/time.h>
  ts.tv_sec = milli_seconds / 1000;		// Conversion a ms de la variable en s
  ts.tv_nsec = (milli_seconds % 1000) * 1000000;	// Conversion a ms de la variable en us
  nanosleep(&ts, NULL);		// Toma el tiempo calculado en ts como estructura y sera el que utilice para la detencion del programa
}

static void StoreInStream(const Magick::Image &img, int delay_time_us,	// Carga de ficheros y tiempo de retraso entre los mismos
                          bool do_center,	// Centrado de la imagen
                          rgb_matrix::FrameCanvas *scratch,		// Instruccion para exportar el fichero a una fuente externa
                          rgb_matrix::StreamWriter *output) {	// ""
  scratch->Clear();
  const int x_offset = do_center ? (scratch->width() - img.columns()) / 2 : 0;	// En caso de que se pida centrar la imagen,
  const int y_offset = do_center ? (scratch->height() - img.rows()) / 2 : 0;	// se modifica la posicion en x e y
  for (size_t y = 0; y < img.rows(); ++y) {		// Barrido en el eje y
    for (size_t x = 0; x < img.columns(); ++x) {	// Barrido en el eje x
      const Magick::Color &c = img.pixelColor(x, y);	// Obtencion del color (c), x e y son variables de tamaño de la matriz
      if (c.alphaQuantum() < 256) {								// Establece el color de cada pixel
        scratch->SetPixel(x + x_offset, y + y_offset,	// Posicion del pixel a colorear
                          ScaleQuantumToChar(c.redQuantum()),		// Rojo, conversion de cantidad a cadena
                          ScaleQuantumToChar(c.greenQuantum()),		// Verde, ""
                          ScaleQuantumToChar(c.blueQuantum()));		// Azul, ""
      }
    }
  }
  output->Stream(*scratch, delay_time_us);
}

static void CopyStream(rgb_matrix::StreamReader *r,
                       rgb_matrix::StreamWriter *w,
                       rgb_matrix::FrameCanvas *scratch) {
  uint32_t delay_us;
  while (r->GetNext(scratch, &delay_us)) {
    w->Stream(*scratch, delay_us);
  }
}

// Carga la imagen actual
// La escala, de forma que encaje en ancho y largo, guarda el valor en result.
static bool LoadImageAndScale(const char *filename,		// Nombre del archivo
                              int target_width, int target_height,	// Ancho y largo objetivo
                              bool fill_width, bool fill_height,	// Llenado vertical/horizontal
                              std::vector<Magick::Image> *result,	// Toma los valores del fichero y lo almacena en *result (vector)
                              std::string *err_msg) {	// Error como cadena de caracteres
  std::vector<Magick::Image> frames;	// Genera un vector (frames) que contiene las imagenes cargadas
  try {									// Estructura de excepcion. El programa leera imagenes hasta que se produzca una excepcion.
    readImages(&frames, filename);		
  } catch (std::exception& e) {
    if (e.what()) *err_msg = e.what();	// En este caso, la excepcion sera la señal de error.
    return false;						// Devuelve false para detener el programa
  }
  if (frames.size() == 0) {	// Deteccion de error
    fprintf(stderr, "Imagen no encontrada.");	// No se encuentra la imagen
    return false;
  }

  // Combina una secuencia de imagenes, util para Gif
  if (frames.size() > 1) {
    Magick::coalesceImages(result, frames.begin(), frames.end());	// Toma el inicio y el final de los frames aportados y los guarda en result
  } else {
    result->push_back(frames[0]);   // En caso de ser una imagen unica, result toma el valor 0
  }

  const int img_width = (*result)[0].columns();	// Ancho de la imagen almacenada en result
  const int img_height = (*result)[0].rows();	// Altura de la imagen almacenada en result
  const float width_fraction = (float)target_width / img_width;	// Escalado horizontal
  const float height_fraction = (float)target_height / img_height;	// Escalado vertical
  if (fill_width && fill_height) {	// En caso de que se pida, la imagen se adapta en eje x e y al maximo de su capacidad
    const float larger_fraction = (width_fraction > height_fraction)	// Condicion ancho > largo
      ? width_fraction		// En caso de que se cumpla, larger_fraction toma el valor de width_fraction
      : height_fraction;	// En caso contrario, toma el valor de height_fraction
    target_width = (int) roundf(larger_fraction * img_width);	// Redondeo del escalado de la imagen horizontal
    target_height = (int) roundf(larger_fraction * img_height);	// Redondeo del escalado de la imagen vertical
  }
  else if (fill_height) {
    // Escalado de la imagen horizontal
    target_width = (int) roundf(height_fraction * img_width);	// Calculo de ancho como el redondeo del producto de ancho y escala unitaria
  }
  else if (fill_width) {
    // Escalado de la imagen vertical
    target_height = (int) roundf(width_fraction * img_height);	// Calculo de alto "" alto y escala unitaria
  }

  for (size_t i = 0; i < result->size(); ++i) {	// Asigna a cada elemento de result de tipo imagen el tamaño obtenido previamente
    (*result)[i].scale(Magick::Geometry(target_width, target_height));	// como target_height y target_width
  }

  return true;
}

// Funcion para la muestra de animaciones Gif
void DisplayAnimation(const FileInfo *file,		// Declara la variable file, de tipo clase FileInfo
                      RGBMatrix *matrix, FrameCanvas *offscreen_canvas,
                      int vsync_multiple) {		// Sincronizacion vertical
	// La duracion de muestra del archivo dependera de la naturaleza del mismo, Gif o imagen
  const tmillis_t duration_ms = (file->is_multi_frame				// Condicion: ¿Tiene el fichero mas de 1 frame?
                                 ? file->params.anim_duration_ms	// Si se cumple dicha condicion, toma duracion de Gif
                                 : file->params.wait_ms);			// No se cumple dicha condicion, toma duracion de imagen
  rgb_matrix::StreamReader reader(file->content_stream);
  int loops = file->params.loops;	// La variable loops toma el valor aportado por loops dentro de parametros 
  const tmillis_t end_time_ms = GetTimeInMillis() + duration_ms;	// Tiempo de finalizacion de muestra: hora actual + duracion de muestra
  const tmillis_t override_anim_delay = file->params.anim_delay_ms;	// El tiempo de anulado de ejecucion toma el valor del tiempo de retraso
  for (int k = 0;
       (loops < 0 || k < loops)	// OR logico de ambas condiciones
         && !interrupt_received	// No se recibe interrupcion
         && GetTimeInMillis() < end_time_ms;	// El tiempo de ejecucion es menor al de finalizacion
       ++k) {
    uint32_t delay_us = 0;	// Resetea el valor de el retraso entre imagenes en us
    while (!interrupt_received && GetTimeInMillis() <= end_time_ms	// Mientras que no se reciba la señal de interrupcion y el tiempo de ejecucion
           && reader.GetNext(offscreen_canvas, &delay_us)) {		// sea inferior al tiempo de finalizacion y al de retraso entre archivos
      const tmillis_t anim_delay_ms =	// El retraso entre animaciones toma el valor del anulado de ejecucion si este es > 0, o el valor de retraso
        override_anim_delay >= 0 ? override_anim_delay : delay_us / 1000;	// por defecto si este es < 0, es decir, ha terminado
      const tmillis_t start_wait_ms = GetTimeInMillis();	// Establece en este instante el tiempo de inicio como tiempo actual
      offscreen_canvas = matrix->SwapOnVSync(offscreen_canvas, vsync_multiple);	// 
      const tmillis_t time_already_spent = GetTimeInMillis() - start_wait_ms;	// Tiempo pasado desde la ejecucion se calcula como tiempo actual - tiempo de inicio
      SleepMillis(anim_delay_ms - time_already_spent);	// Tiempo de parada de la funcion main establecido como tiempo de retraso menos tiempo de ejecucion
    }
    reader.Rewind();
  }
}

static int usage(const char *progname) {
  fprintf(stderr, "usage: %s [options] <image> [option] [<image> ...]\n",
          progname);

  fprintf(stderr, "Opciones:\n"
          "\t-O<streamfile>            : Output to stream-file instead of matrix (Don't need to be root).\n"
          "\t-C                        : Centra imagenes.\n"

          "\nEstas opciones afectan a las imagenes siguientes en la linea de comandos:\n"
          "\t-w<seconds>               : Imagen normal: "
          "Tiempo de espera entre imagenes en segundos (por defecto: 1.5).\n"
          "\t-t<seconds>               : "
          "Para animaciones: Se detiene tras este tiempo.\n"
          "\t-l<loop-count>            : "
          "Para animaciones: numero de repeticiones para un ciclo completo.\n"
          "\t-D<animation-delay-ms>    : "
          "Para animaciones: anula el retraso entre frames dado en\n"
          "\t                            gif/stream animation con este parametro. Usar -1 para el valor por defecto.\n"

          "\nOpciones que afectan a la muestra de multiples imagenes:\n"
          "\t-f                        : "
          "Ciclo perpetuo entre todos los ficheros de la linea de comandos.\n"
          "\t-s                        : Si se aportan varias imagenes, se mezclan al mostrarse.\n"
          "\nDisplay Options:\n"
          "\t-V<vsync-multiple>        : Expert: Only do frame vsync-swaps on multiples of refresh (default: 1)\n"
          );

  fprintf(stderr, "\nOpciones generales LED matrix:\n");
  rgb_matrix::PrintMatrixFlags(stderr);

  fprintf(stderr,
          "\nTiempo entre cambio de archivos: "
          "-w para imagenes estaticas; -t/-l para animaciones\n"
          "Gifs animados: En caso de recibir -l y -t, "
          "el primero en terminar, determina la duracion.\n");

  fprintf(stderr, "\nLos parametros -w, -l y -t se aplican a las siguientes imagenes "
          "hasta que aparezca una nueva instancia de una de ellas.\n"
          "Puedes aplicar diferentes opciones de tiempo para diferentes imagenes.\n");

  return 1;
}

int main(int argc, char *argv[]) {	// Programa principal, argumentos de entrada representan la cantidad de argumentos que pretendemos pasarle a main
  Magick::InitializeMagick(*argv);	// Inicializacion de la biblioteca Magick

  RGBMatrix::Options matrix_options;	// Carga las opciones de matriz de led-matrix.h
  rgb_matrix::RuntimeOptions runtime_opt;	// Carga las opciones de inicializacion de matriz de led-matrix.h
  if (!rgb_matrix::ParseOptionsFromFlags(&argc, &argv,	// La funcion ParseOptionsFromFlags devuelve True si detecta un fallo en los argumentos
                                         &matrix_options, &runtime_opt)) {
    return usage(argv[0]);	// En caso de detectar un error vuelve a la pedida de argumentos
  }

  int vsync_multiple = 1;	// Sincronizacion vertical, inicializacion de la variable
  bool do_forever = false;	// Parametros de bucle perpetuo, centrado de imagenes y mezcla false por defecto
  bool do_center = false;
  bool do_shuffle = false;

	// Comando que asociara a cada fichero las opciones que se le hayan asignado previamente.
  std::map<const void *, struct ImageParams> filename_params;

	// Establece valores por defecto para img_param para todas las imagenes cargadas
  ImageParams img_param;
  for (int i = 0; i < argc; ++i) {
    filename_params[argv[i]] = img_param;
  }

  const char *stream_output = NULL;

  int opt;	// Declaracion de la variable opt
  while ((opt = getopt(argc, argv, "w:t:l:fr:c:P:LhCR:sO:V:D:")) != -1) {	// Parametros para la muestra de ficheros
    switch (opt) {	// Estructura de posibles casos para cada argumento de entrada para opt
    case 'w':	// Tiempo de espera entre imagenes
      img_param.wait_ms = roundf(atof(optarg) * 1000.0f); // Conversion a ms, cadena a doble y redondeo
      break;
    case 't':	// Tiempo de duracion de animaciones
      img_param.anim_duration_ms = roundf(atof(optarg) * 1000.0f);	// Conversion a ms, cadena a doble y redondeo
      break;
    case 'l':	// Numero de ciclos a repetir
      img_param.loops = atoi(optarg);	// Convierte cadena a entero
      break;
    case 'D':	// Retraso entre animaciones
      img_param.anim_delay_ms = atoi(optarg);	// Convierte cadena a entero
      break;
    case 'f':	// Bucle perpetuo entre archivos proporcionados
      do_forever = true;
      break;
    case 'C':	// Centrado de imagenes
      do_center = true;
      break;
    case 's':	// Mezclado de imagenes
      do_shuffle = true;
      break;
    case 'r':	// Opcion paa cambiar el numero de filas
      fprintf(stderr, "Utilizar --led-rows=%s en vez de esta opcion.\n",
              optarg);
      matrix_options.rows = atoi(optarg);	// Convierte cadena a entero
      break;
    case 'c':	// Opcion para cambiar el numero de columnas 
      fprintf(stderr, "Utilizar --led-chain=%s en vez de esta opcion.\n",
              optarg);
      matrix_options.chain_length = atoi(optarg);	// Convierte cadena a entero
      break;
    case 'P':
      matrix_options.parallel = atoi(optarg);	// Convierte cadena a entero
      break;
    case 'L':	// Opciones de mapeado y encadenado de la matriz
      fprintf(stderr, "Utilizar --led-pixel-mapper=\"U-mapper\" --led-chain=4\nen vez de esta opcion.\n");
      return 1;
      break;
    case 'R':	// Opcion de rotacion del fichero
      fprintf(stderr, "-R es una opcion obsoleta. "
              "Utilizar --led-pixel-mapper=\"Rotate:%s\" en vez de esta opcion.\n", optarg);
      return 1;
      break;
    case 'O':	// Caso de que se pretenda exportar el fichero fuera de la matriz led
      stream_output = strdup(optarg);	
      break;
    case 'V':
      vsync_multiple = atoi(optarg);	// Convierte cadena a entero
      if (vsync_multiple < 1) vsync_multiple = 1;	// Opcion de VSync
      break;
    case 'h':	// Caso de que no se incluya ningun argumento
    default:
      return usage(argv[0]);
    }

    // Empezando por el archivo actual, establece las condiciones de los archivos restantes al ultimo cambio
    for (int i = optind; i < argc; ++i) {
      filename_params[argv[i]] = img_param;
    }
  }

  const int filename_count = argc - optind;	// Conteo de archivos de entrada
  if (filename_count == 0) {	// En caso de no recibir ningun archivo
    fprintf(stderr, "No se ha encontrado ningun fichero compatible.\n");	
    return usage(argv[0]);	// Vuelve a usage, para que se vuelva a introducir la informacion deseada
  }

  // Preparacion de la matriz
  runtime_opt.do_gpio_init = (stream_output == NULL);
  RGBMatrix *matrix = CreateMatrixFromOptions(matrix_options, runtime_opt);	// Valores de la matriz
  if (matrix == NULL)
    return 1;

  FrameCanvas *offscreen_canvas = matrix->CreateFrameCanvas();	// ASOCIACION DE MATRIZ APAGADA A INICIALIZACION DE MATRIZ
																// DENTRO DE led-matrix.h, linea 236

  printf("Tamaño: %dx%d. Mapeado de hardware GPIO: %s\n",
         matrix->width(), matrix->height(), matrix_options.hardware_mapping);	// Muestra parametros de tamaño en pantalla

  // These parameters are needed once we do scrolling.
  const bool fill_width = false;
  const bool fill_height = false;

  // En caso de pedirse salida externa, establece la salida externa objetivo.
  rgb_matrix::StreamIO *stream_io = NULL;
  rgb_matrix::StreamWriter *global_stream_writer = NULL;
  if (stream_output) {
    int fd = open(stream_output, O_CREAT|O_WRONLY, 0644);
    if (fd < 0) {
      perror("No se ha podido abrir la salida externa objetivo");
      return 1;
    }
    stream_io = new rgb_matrix::FileStreamIO(fd);
    global_stream_writer = new rgb_matrix::StreamWriter(stream_io);
  }

  const tmillis_t start_load = GetTimeInMillis();
  fprintf(stderr, "Cargando %d archivos...\n", argc - optind);
  //Se preparan los ficheron antes de mostrarlos, para evitar la ralentizacion del sistema
  std::vector<FileInfo*> file_imgs;
  for (int imgarg = optind; imgarg < argc; ++imgarg) {
    const char *filename = argv[imgarg];
    FileInfo *file_info = NULL;

    std::string err_msg;
    std::vector<Magick::Image> image_sequence;
    if (LoadImageAndScale(filename, matrix->width(), matrix->height(),
                          fill_width, fill_height, &image_sequence, &err_msg)) {
      file_info = new FileInfo();
      file_info->params = filename_params[filename];
      file_info->content_stream = new rgb_matrix::MemStreamIO();
      file_info->is_multi_frame = image_sequence.size() > 1;
      rgb_matrix::StreamWriter out(file_info->content_stream);
      for (size_t i = 0; i < image_sequence.size(); ++i) {
        const Magick::Image &img = image_sequence[i];
        int64_t delay_time_us;		// Declara el tiempo entre muestra de archivos en us
        if (file_info->is_multi_frame) {	// Comprobacion de si el archivo se trata de una animacion
          delay_time_us = img.animationDelay() * 10000; // En caso de tratarse de un gif, lo muestra 10000 us
        } else {
          delay_time_us = file_info->params.wait_ms * 1000;  // En caso de ser una imagen, lo muestra 1500*1000 us
        }
        if (delay_time_us <= 0) delay_time_us = 100 * 1000;  // Si el tiempo entre muestra de archivos es inferior a 0.1s, lo fija a este valor
        StoreInStream(img, delay_time_us, do_center, offscreen_canvas,
                      global_stream_writer ? global_stream_writer : &out);
      }
    } else {
      // En caso de no resultar ser una imagen, prueba con una fuente externa
      int fd = open(filename, O_RDONLY);
      if (fd >= 0) {
        file_info = new FileInfo();
        file_info->params = filename_params[filename];
        file_info->content_stream = new rgb_matrix::FileStreamIO(fd);
        StreamReader reader(file_info->content_stream);
        if (reader.GetNext(offscreen_canvas, NULL)) {  // header+size ok
          file_info->is_multi_frame = reader.GetNext(offscreen_canvas, NULL);
          reader.Rewind();
          if (global_stream_writer) {
            CopyStream(&reader, global_stream_writer, offscreen_canvas);
          }
        } else {
          err_msg = "No se puede leer como una imagen compatible";
          delete file_info->content_stream;
          delete file_info;
          file_info = NULL;
        }
      }
    }

    if (file_info) {
      file_imgs.push_back(file_info);
    } else {
      fprintf(stderr, "%s saltado: No se ha podido abrir (%s)\n",
              filename, err_msg.c_str());
    }
  }

  if (stream_output) {
    delete global_stream_writer;
    delete stream_io;
    if (file_imgs.size()) {
      fprintf(stderr, "Realizado: Salida externa %s; "
              "ahora puede abrirse con led-image-viewer con la misma configuracion de panel\n", stream_output);
    }
    if (do_shuffle)
      fprintf(stderr, "Nota: -s (mezcla) no tiene efecto al generarse archivos externos.\n");
    if (do_forever)
      fprintf(stderr, "Nota: -f (bucle perpetuo) no tiene efecto al generarse archivos externos.\n");
    // Si llega a este punto no se mostrara nada en pantalla
    return 0;
  }

  if (file_imgs.empty()) {
    // Caso de que las imagenes no puedan ser representadas
    fprintf(stderr, "No se ha codido cargar la imagen.\n"); // Muestra por pantalla el fallo
    return 1;	
  } else if (file_imgs.size() == 1) {
    // Imagen unica
    file_imgs[0]->params.wait_ms = distant_future;
  } else {
    for (size_t i = 0; i < file_imgs.size(); ++i) {
      ImageParams &params = file_imgs[i]->params;
      // Forever animation ? Set to loop only once, otherwise that animation
      // would just run forever, stopping all the images after it.
      if (params.loops < 0 && params.anim_duration_ms == distant_future) {
        params.loops = 1;
      }
    }
  }

  fprintf(stderr, "Loading took %.3fs; now: Display.\n",
          (GetTimeInMillis() - start_load) / 1000.0);

  signal(SIGTERM, InterruptHandler);	// Termina la señal, libreria propia de c
  signal(SIGINT, InterruptHandler);		// Interrumpe la señal, libreria propia de c
										// Ambas instrucciones hacen que la variable interrupt_received se ponga a true

  do {
    if (do_shuffle) {	// Condicion para mezclado de imagenes
      std::random_shuffle(file_imgs.begin(), file_imgs.end()); // Instruccion para mezclado de imagenes
    }
    for (size_t i = 0; i < file_imgs.size() && !interrupt_received; ++i) {	// Para todas las imagenes, mientras no se reciba señal de interrupcion
      DisplayAnimation(file_imgs[i], matrix, offscreen_canvas, vsync_multiple);	// Muestra las imagenes en la matriz
    }
  } while (do_forever && !interrupt_received);	// Bucle perpetuo mientras no se reciba interrupcion

  if (interrupt_received) {	// En caso de recibir señal de interrupcion, se detiene el programa
    fprintf(stderr, "Señal recibida. Saliendo.\n");	// Aviso de interrupcion
  }

  // Animacion terminada. Apagado de la matriz
  matrix->Clear();	// Limpiado de la matriz, puesta a 0 de todos los pixeles
  delete matrix;	// Borra los datos de la matriz para la puesta a 0

  return 0;
}
