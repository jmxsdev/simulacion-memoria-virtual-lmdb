# Guion para el video — Proyecto N.° 13

**Simulación de Memoria Virtual con LMDB**
Jesús Alberto Mora Lozada — C.I. 28.480.557 — Arquitectura del Computador

Este guion acompaña a `diapositivas/diapositivas.tex` (17 diapositivas). Está
pensado para un video de **20 a 25 minutos** (el enunciado pide entre 15 y 30),
incluyendo la demostración de ejecución y el análisis y discusión de resultados.

## Cómo usar este guion

- El texto en cursiva es **lo que dices**; puedes adaptarlo a tu forma de hablar.
- Los tiempos son una guía, no una obligación.
- Ten el proyecto compilado y el dataset de humo listo **antes** de grabar, para
  no perder tiempo en vivo.
- Habla pausado y mira a la cámara en las transiciones. No leas la diapositiva
  palabra por palabra: explícala.

## Reparto de tiempo sugerido

| Bloque | Diapositivas | Tiempo |
| --- | --- | --- |
| Apertura y problema | 1–4 | 4 min |
| Técnicas y diseño | 5–8 | 5 min |
| Metodología y demostración | 9 + demo | 5 min |
| Resultados | 10–14 | 6 min |
| Conclusiones y cierre | 15–17 | 3 min |
| **Total** | | **≈ 23 min** |

---

## Diapositiva 1 — Portada

**Duración:** 40 segundos.

**Guion:**

> *Buenos días. Mi nombre es Jesús Alberto Mora Lozada, cédula 28.480.557, y les
> presento el Proyecto N.° 13 de la asignatura Arquitectura del Computador:
> "Simulación de Memoria Virtual con LMDB". En este video voy a explicar el
> problema que aborda el proyecto, las técnicas computacionales que utilicé, y
> el análisis y comparación de los resultados obtenidos.*

**Transición:** *"Empecemos por el problema."*

---

## Diapositiva 2 — Contenido

**Duración:** 30 segundos.

**Guion:**

> *El video está organizado en cinco partes: primero el problema y los
> objetivos; después las técnicas computacionales, es decir, la memoria virtual,
> LMDB y el árbol B+; luego el diseño del sistema y la metodología de medición;
> a continuación los resultados, con la demostración en vivo; y cierro con las
> conclusiones.*

**Transición:** *"Vamos con el problema."*

---

## Diapositiva 3 — El Problema

**Duración:** 1 minuto 30 segundos.

**Guion:**

> *El enunciado nos pide estudiar una técnica de los capítulos de Memoria y
> Discos, implementarla en C++ y comparar el rendimiento en distintos
> escenarios. El Proyecto N.° 13 propone usar LMDB para gestionar grandes
> volúmenes de datos como si fuera memoria secundaria.*
>
> *La pregunta que quise responder es concreta: cuando los datos no caben en
> RAM, ¿cuánto cuesta realmente traerlos del disco? Esa diferencia es la
> paginación por demanda, el mecanismo central de la memoria virtual.*
>
> *Elegí el dominio de las apuestas de lotería venezolana porque tiene
> exactamente el perfil que necesitaba: registros pequeños y muy numerosos, con
> consultas puntuales y por rango, que es justo lo que un árbol B+ hace bien.*

**Punto clave a enfatizar:** el proyecto no es "usar una base de datos"; es
**medir el costo de la memoria virtual**.

**Transición:** *"Con eso claro, estos son los objetivos."*

---

## Diapositiva 4 — Objetivos

**Duración:** 1 minuto.

**Guion:**

> *El objetivo general es implementar un sistema en C++ sobre LMDB que gestione
> grandes volúmenes de datos como memoria secundaria, mida la latencia y
> demuestre la paginación por demanda.*
>
> *Los objetivos específicos son cinco: diseñar claves binarias que exploten el
> orden del árbol B+; construir un generador determinista de datasets a escala;
> medir escritura, latencia y fallos de página mayor y menor; comparar el
> acceso con caché tibia, es decir, desde RAM, contra caché fría, desde disco; y
> validar los agregados contra un manifiesto de verdad absoluta.*

**Transición:** *"Ahora veamos las técnicas que hacen posible todo esto."*

---

## Diapositiva 5 — Técnica 1: memoria virtual y `mmap`

**Duración:** 1 minuto 30 segundos.

**Guion:**

> *La primera técnica es la memoria virtual. El sistema operativo divide la
> memoria en páginas de 4 kilobytes. Cuando un programa accede a una página que
> no está en RAM, ocurre un fallo de página: el kernel la trae del disco y
> reanuda la ejecución. Si la página ya estaba en memoria, el fallo es menor y
> es barato; si hay que leerla del dispositivo, el fallo es mayor y es caro.*
>
> *La traducción de direcciones virtuales a físicas la hace la tabla de páginas,
> y se acelera con la TLB. Todo esto está en la sección 5.4 del libro de
> Patterson y Hennessy.*
>
> *Lo interesante es cómo lo usa LMDB: no copia los datos a un caché propio,
> sino que mapea el archivo a memoria con mmap. Entonces, leer una clave es
> literalmente leer memoria, y el caché de páginas del sistema operativo ES el
> caché de la base de datos.*

**Transición:** *"Y eso nos lleva a LMDB y al árbol B+."*

---

## Diapositiva 6 — Técnica 2: LMDB y el árbol B+

**Duración:** 1 minuto 30 segundos.

**Guion:**

> *LMDB es una base de datos clave-valor: un diccionario que vive en disco. Sus
> características son: un solo archivo mapeado a memoria, sin caché propio,
> con un árbol B+ copia-en-escritura, MVCC para lecturas sin bloqueos, y una
> restricción importante: admite un solo escritor a la vez.*
>
> *El árbol B+ es el índice. Es un árbol de abanico alto: cada nodo es una
> página con muchísimas claves, así que con tres o cuatro niveles se indexan
> miles de millones de entradas. Además, las hojas están enlazadas entre sí, y
> eso es lo que permite recorrer un rango de forma secuencial y rápida.*

**Punto clave:** el B+tree es lo que hace que buscar y recorrer rangos sean
baratos en disco.

**Transición:** *"Pero hay un detalle que hay que cuidar: el orden de las
claves."*

---

## Diapositiva 7 — Técnica 3: claves big-endian

**Duración:** 1 minuto.

**Guion:**

> *LMDB compara las claves byte a byte, como texto, con memcmp. Si guardamos un
> número en little-endian, el orden de los bytes no coincide con el orden
> numérico, y los barridos por rango devuelven resultados incorrectos. Por eso
> la regla del proyecto es que todo entero multibyte dentro de una clave va en
> big-endian. Así, el orden lexicográfico de los bytes es exactamente el orden
> numérico.*
>
> *En el proyecto, la clave de la base principal es: un byte de lotería, ocho
> bytes de marca de tiempo en big-endian, dos de taquilla y dos de secuencia.*

**Transición:** *"Con eso, veamos cómo quedó diseñado el sistema."*

---

## Diapositiva 8 — Diseño: sub-bases e índices

**Duración:** 1 minuto 30 segundos.

**Guion:**

> *El sistema usa un solo entorno LMDB con cinco sub-bases. La base principal,
> bets, guarda el registro maestro de cada jugada y está ordenada por fecha. Las
> otras cuatro responden a patrones de consulta distintos: i_animalito para
> frecuencia y rachas por figura, i_terminal para frecuencia por terminación,
> i_taquilla para el replay de un punto de venta, y draws para los resultados de
> los sorteos.*
>
> *Cada jugada se escribe en la base maestra y en los índices que le
> corresponden. Es el principio de los índices secundarios: pagar espacio y
> tiempo de escritura para ganar velocidad de lectura.*

**Transición:** *"Ahora, ¿cómo medí todo esto?"*

---

## Diapositiva 9 — Metodología: tibio vs frío

**Duración:** 1 minuto 30 segundos.

**Guion:**

> *La metodología se apoya en una comparación: caché tibia contra caché fría. En
> tibia, las páginas están en RAM. En fría, las libero antes de medir.*
>
> *Para liberar el caché sin ser root uso madvise con MADV_DONTNEED sobre la
> región de memoria mapeada del archivo, que localizo en /proc/self/maps. La
> alternativa clásica, que es escribir en drop_caches, exige privilegios de
> administrador; esta no.*
>
> *Los fallos de página los mido leyendo los contadores minflt y majflt de
> /proc/self/stat antes y después de cada corrida, y restando. Es portable y no
> necesita permisos especiales.*

**Transición:** *"Antes de ver los números, les muestro el proyecto
funcionando."*

---

## Demostración en vivo (entre las diapositivas 9 y 10)

**Duración:** 4 a 5 minutos.

**Preparación previa:** ten el proyecto compilado y el dataset de humo
descomprimido. Si el video se graba sin conexión, no dependas de descargar nada.

**Guion mientras ejecutas:**

> *Este es el proyecto. Primero compilo desde cero con make, para mostrar que
> compila limpio, sin advertencias.*

```sh
make clean && make all
```

> *Ahora genero el dataset de humo. El generador es determinista: con la misma
> semilla produce exactamente el mismo archivo, byte a byte. Son un millón de
> jugadas.*

```sh
make smoke
```

> *Y valido los agregados contra el manifiesto que escribe el generador. Deben
> pasar las 149 comprobaciones.*

```sh
./bin/query --db data/smoke.lmdb validate
```

> *Con info puedo ver la estructura interna: cuántas entradas, cuántas hojas y
> ramas tiene el árbol B+, y su profundidad. Fíjense en que la profundidad es de
> solo tres niveles para un millón de claves.*

```sh
./bin/query --db data/smoke.lmdb info
```

> *Hago un lookup puntual con una clave real.*

```sh
./bin/query --db data/smoke.lmdb lookup --lottery 0 --ts 1577838271 --taquilla 46 --seq 0
```

> *Ahora la medición estrella: el mismo benchmark de lookups en modo tibio y en
> modo frío. En tibio las páginas están en RAM; en frío las acabo de liberar.
> Comparen el throughput y, sobre todo, los fallos de página mayores.*

```sh
./bin/bench read-lookup --db data/smoke.lmdb --n 200000
./bin/bench read-lookup --db data/smoke.lmdb --n 200000 --cold
```

**Consejo:** si el número frío sale muy distinto al de las diapositivas,
explícalo: *"el throughput frío depende del estado del disco y del caché en ese
momento; lo estable es el conteo de fallos de página, que ronda los 83 mil en
el dataset de 10 millones"*.

**Transición:** *"Con el proyecto funcionando, veamos los resultados."*

---

## Diapositiva 10 — Resultado 1: escritura ordenada vs aleatoria

**Duración:** 1 minuto 30 segundos.

**Guion:**

> *El primer resultado compara escribir con claves ordenadas, usando
> MDB_APPEND, contra claves aleatorias. Con un millón de registros, la escritura
> ordenada alcanza 1,94 millones de registros por segundo, frente a 226 mil de
> la aleatoria: es 8,6 veces más rápida. Y el archivo final ocupa 118 megabytes
> contra 398: 3,4 veces menos espacio.*
>
> *La lección es que el orden de las claves no es cosmético. Con claves
> ascendentes, cada inserción cae en el borde derecho del árbol y una misma
> página se llena de corrido. Con claves aleatorias, cada inserción abre o
> divide páginas dispersas.*

**Transición:** *"Pero escribir rápido no lo es todo: ¿qué pasa con muchas
taquillas escribiendo a la vez?"*

---

## Diapositiva 11 — Resultado 2: escritura concurrente

**Duración:** 1 minuto 30 segundos.

**Guion:**

> *Este benchmark simula varias taquillas escribiendo en paralelo. Cada taquilla
> es un hilo. Con una sola taquilla alcanzo 1,34 millones de registros por
> segundo; con dieciséis, el throughput baja a 943 mil y el tiempo de commit se
> duplica. Es una caída al 70 por ciento.*
>
> *¿Por qué? Porque LMDB admite un solo escritor a la vez. Añadir hilos no
> multiplica al escritor: solo hace que esperen. Es la ley de Amdahl. La
> solución real no es añadir hilos, sino particionar los datos, por ejemplo en
> varios entornos por rango de tiempo o por taquilla, o agrupar escrituras en
> lotes grandes.*

**Transición:** *"Y ahora la parte que da nombre al proyecto: el costo de la
paginación."*

---

## Diapositiva 12 — Resultado 3: el costo de la paginación (10M)

**Duración:** 2 minutos.

**Guion:**

> *Con un dataset de diez millones de jugadas, un gigabyte y medio, comparé
> lookups y barridos en tibio y en frío. Un lookup tibio hace unas 900 mil
> operaciones por segundo. En frío cae a unas 3.400: es 267 veces más lento.*
>
> *Lo más importante no es el número, sino la evidencia: en modo frío se
> registraron 83.251 fallos de página mayores, es decir, lecturas reales al
> disco, frente a cero en tibio. El tiempo extra corresponde exactamente a esos
> accesos.*
>
> *En cambio, el barrido secuencial apenas se afecta: cae solo 1,4 veces. ¿Por
> qué? Porque el kernel detecta el acceso secuencial y activa la relectura
> anticipada: trae páginas por delante de que se necesiten. Los lookups
> aleatorios no pueden anticiparse, y por eso sufren tanto.*

**Punto clave:** esta es la demostración empírica de la memoria virtual.

**Transición:** *"¿Y si escalamos todavía más?"*

---

## Diapositiva 13 — Resultado 4: escalamiento a 100 millones

**Duración:** 1 minuto 30 segundos.

**Guion:**

> *Para cerrar el criterio de escala, generé y validé cien millones de jugadas:
> quince coma cinco gigabytes en disco, unos 163 bytes por jugada, con las 149
> comprobaciones de agregados aprobadas. El árbol B+ de la base principal queda
> en cuatro niveles: un nivel más que con un millón, para cien veces más datos.
> Ese es el punto de operación del experimento.*
>
> *Un detalle honesto: en mi máquina, de 32 gigabytes de RAM, ese dataset cabe;
> por eso el efecto frío no se produce por presión de memoria, sino forzándolo
> con madvise. En una máquina de 8 o 16 gigabytes, la paginación sería
> inevitable.*

**Transición:** *"Y una última lección sobre optimización."*

---

## Diapositiva 14 — Resultado 5: optimizar también es no interferir

**Duración:** 1 minuto 30 segundos.

**Guion:**

> *Probé darle pistas al kernel sobre el patrón de acceso. Con MADV_RANDOM antes
> de los lookups, esperaba mejorar el rendimiento; ocurrió lo contrario: empeoró
> 6,3 veces. ¿Por qué? Porque desactiva la relectura anticipada, que estaba
> ayudando aunque los accesos fueran aleatorios. Y MADV_SEQUENTIAL antes de los
> barridos solo mejoró un 12 por ciento, porque el kernel ya detecta el acceso
> secuencial solo.*
>
> *La lección es contraintuitiva: no interferir con las heurísticas del kernel
> es, en sí mismo, una optimización.*

**Transición:** *"Con esto, llegamos a las conclusiones."*

---

## Diapositiva 15 — Conclusiones

**Duración:** 2 minutos.

**Guion:**

> *Primero: el dominio de apuestas resultó adecuado. Registros pequeños,
> ordenables y con consultas de negocio reales.*
>
> *Segundo: el diseño de claves big-endian habilita MDB_APPEND y los barridos
> por rango, y esa decisión vale 8,6 veces de rendimiento.*
>
> *Tercero: los benchmarks confirman la teoría. Los accesos aleatorios son 267
> veces más lentos en frío, mientras que los secuenciales apenas se afectan
> gracias a la relectura anticipada.*
>
> *Cuarto: medí 83.251 fallos de página mayores. La paginación por demanda no es
> una idea abstracta: existe y se cuantifica.*
>
> *Quinto: LMDB tiene un solo escritor. Para escalar a miles de taquillas hay
> que particionar, no añadir hilos.*
>
> *Y sexto: el proyecto se validó a escala real, con cien millones de jugadas y
> quince coma cinco gigabytes, con los agregados exactos.*

**Transición:** *"Estas son las referencias."*

---

## Diapositiva 16 — Referencias

**Duración:** 30 segundos.

**Guion:**

> *El marco teórico se apoya en el libro de Patterson y Hennessy, capítulos 5 y
> 6, en particular la sección 5.4 de memoria virtual. La documentación de LMDB y
> las páginas del manual de Linux para mmap y madvise completan las
> referencias. El repositorio con todo el código está en el enlace que aparece
> en pantalla.*

**Transición:** *"Para cerrar, el análisis y la discusión."*

---

## Diapositiva 17 — Gracias y discusión

**Duración:** 2 a 3 minutos.

**Guion (análisis y discusión final):**

> *Como análisis final, el proyecto demuestra tres cosas.*
>
> *Primera: que LMDB es un excelente caso de estudio de memoria virtual, porque
> no oculta el mecanismo: el caché es el del sistema operativo y los fallos de
> página son observables directamente.*
>
> *Segunda: que el diseño de datos importa tanto como el motor. Ordenar las
> claves y elegir bien los índices cambió el rendimiento en un orden de
> magnitud.*
>
> *Tercera: que hay límites que no se resuelven con más hardware. El escritor
> único de LMDB es una decisión de diseño que obliga a particionar los datos
> cuando se escala.*
>
> *Como trabajo futuro, me gustaría probar el particionado por rango de tiempo
> con varios entornos LMDB, y medir el escenario de diez mil taquillas sobre esa
> arquitectura. También me gustaría comparar LMDB con SQLite u otro motor, para
> tener una referencia.*
>
> *Con esto cierro la presentación. Muchas gracias por su atención; quedo
> atento a las preguntas.*

---

## Checklist antes de grabar

- [ ] El proyecto compila limpio (`make clean && make all`).
- [ ] El dataset de humo está descomprimido (`gunzip -k data/smoke.lmdb.gz`).
- [ ] La validación da 149/149.
- [ ] Las diapositivas compilan y se ven bien en pantalla.
- [ ] Cierras notificaciones y demás ventanas.
- [ ] Micrófono probado; grabas en un lugar silencioso.
- [ ] Subes el video a YouTube en modo **no listado** (no privado).

## Consejos de grabación

- Graba por bloques (por ejemplo, un bloque por sección) y únelos después; así
  no repites todo si te equivocas.
- Si te trabas, respira y retoma desde la diapositiva, no desde el principio.
- Muestra el cursor o resalta lo que señalas; ayuda a seguir el video.
- Al final, revisa que se escuche bien y que el enlace de YouTube funcione en
  una ventana de incógnito (así confirmas que el modo no listado es visible).
