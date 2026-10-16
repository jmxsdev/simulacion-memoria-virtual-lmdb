# Validación del recurso de ayuda y propuesta de mejora

**Proyecto N.° 13 — Simulación de Memoria Virtual con LMDB**
Jesús Alberto Mora Lozada — C.I. 28.480.557 — Arquitectura del Computador

Este documento cumple con el requisito del enunciado de *«incluir la validación
del recurso correspondiente en el desarrollo del proyecto seleccionado y su
propuesta de mejora al recurso de ayuda: infografía/diapositiva con su
respectivo prompt»*.

## 1. Recurso analizado

Del repositorio `https://github.com/josedevlabs/computer-arch-labs/tree/main/docs/finals`
se tomaron los recursos del Proyecto 13:

- `Proyecto_13_teoria.jpeg` — infografía teórica.
- `Proyecto_13_practica.png` — infografía práctica.

La infografía teórica presenta, en tres bloques:

1. **Bloque 1 — El mecanismo de memory-mapped files (`mmap`):** comparación
   entre E/S tradicional (`read`/`write`, doble copia, buffer cache) y `mmap`
   (zero-copy, single-level store), con un diagrama de hardware secuencial del
   fallo de página (MMU, tabla de páginas, caché de páginas, disco).
2. **Bloque 2 — Arquitectura del motor transaccional LMDB:** el *delegar
   caching y paginación al SO*, MVCC y copia-en-escritura, el árbol B+, y una
   tabla comparativa (`mmap`/LMDB vs. APIs de E/S tradicionales).
3. **Bloque 3 — Panel de datos masivos:** el caso sísmico (propagación de
   ondas) y su transferencia al proyecto 13 mediante el caso médico (biopsias
   digitalizadas, quadtrees, LMDB).

## 2. Validación del recurso en el desarrollo del proyecto

El recurso resultó **directamente aplicable**: describe exactamente la técnica
que el proyecto implementa y mide. La validación se hizo confirmando con
mediciones propias cada afirmación central de la infografía.

| Afirmación del recurso | Validación con nuestro proyecto |
| --- | --- |
| LMDB delega el caché y la paginación al SO | Confirmado: LMDB no tiene caché propio; `bin/query info` y los benchmarks usan el caché de páginas del SO. |
| `mmap` elimina la copia de datos (zero-copy) | Confirmado: las lecturas acceden a memoria virtual directamente; el lookup tibio es de $\approx 0{,}8\,\mu$s. |
| El acceso a una página ausente produce un fallo de página | **Cuantificado**: en el dataset de 10M medimos **83\,251 fallos de página mayores** en modo frío, frente a 0 en tibio. |
| El caché de páginas del SO es el caché real | Confirmado forzando su vaciado con `madvise(MADV_DONTNEED)`: el mismo lookup pasa de $\approx 900$ K a $3{,}4$ K ops/s ($\times 267$). |
| MVCC: lecturas sin bloqueos gracias a copia-en-escritura | Confirmado: los comandos de `query` (solo lectura) corren sin bloquear escrituras. |
| El árbol B+ es la estructura interna | Confirmado: `bin/query info` reporta la altura del B+tree (4 niveles para 100M de jugadas). |
| `mmap`/LMDB tiene baja sobrecarga de CPU frente a la E/S tradicional | Confirmado con el benchmark de escritura ordenada: **1{,}94 M registros/s**. |

## 3. Limitaciones encontradas

El recurso es claro y técnicamente correcto, pero presenta tres omisiones
relevantes para quien lo use para desarrollar el proyecto:

1. **No menciona la restricción de un solo escritor de LMDB.** Es la limitación
   más consecuente en la práctica: limita el escalado concurrente. Nuestro
   `write-concurrent` lo demuestra (el throughput cae a $\times 0{,}70$ al
   pasar de 1 a 16 taquillas, por la ley de Amdahl).
2. **No menciona la obligación del big-endian en las claves.** Como LMDB
   compara claves con `memcmp`, omitir esto produce errores silenciosos en los
   barridos por rango (uno de los tropiezos más comunes).
3. **No incluye resultados medidos.** Los ejemplos (sísmica, médica) son
   ilustrativos, pero un panel con cifras reales de frío vs. tibio haría el
   recurso mucho más didáctico y comprobable.

## 4. Propuesta de mejora

Se propone añadir a la infografía teórica del Proyecto 13:

- Un **bloque 4 «Resultados medidos»** con las cifras reales del proyecto:
  escritura ordenada $\times 8{,}6$; lookup frío $\times 267$; 83\,251 fallos de
  página mayores; 100 M de jugadas (15,5 GB) validadas 149/149.
- Una nota dentro del bloque 2 sobre la **restricción de un solo escritor** y
  su consecuencia (ley de Amdahl), con la estrategia de particionado.
- Una nota en el bloque 1 sobre el **big-endian en las claves** (consecuencia
  de comparar con `memcmp`).
- Reemplazar o complementar el caso sísmico/médico con el **dominio de
  apuestas**, para que el recurso cubra un ejemplo de registros pequeños y
  numerosos (complementa el de datos voluminosos).

## 5. Prompt para regenerar la infografía mejorada

```
Crea una infografía técnica vertical (estilo académico universitario, en
español) titulada "MEMORIA VIRTUAL Y ARCHIVOS MAPEADOS: FUNDAMENTOS DE LMDB",
dividida en cuatro bloques horizontales con encabezados de color:

BLOQUE 1 — EL MECANISMO DE MEMORY-MAPPED FILES (mmap):
Compara "E/S tradicional" (syscalls read/write, doble copia, buffer cache) con
"mmap" (zero-copy, single-level store). Añade un diagrama de hardware secuencial
del fallo de página: CPU -> MMU -> tabla de páginas -> caché de páginas -> disco
-> "Page Fault". Incluye una nota destacada: "Las claves de LMDB se comparan
byte a byte (memcmp); los enteros multi-byte DEBEN ir en BIG-ENDIAN para que el
orden de bytes coincida con el orden numérico".

BLOQUE 2 — ARQUITECTURA DEL MOTOR TRANSACCIONAL LMDB:
Muestra que LMDB delega el caching y la paginación al sistema operativo
(diagrama LMDB <-> OS Kernel <-> Page Cache). Explica MVCC y copia-en-escritura
(lectura concurrente free-wait; escritura copia-en-escritura). Dibuja el árbol
B+ (raíz, nodos internos, hojas enlazadas, nodo de 4 KB). Incluye una nota
destacada sobre la restricción: "LMDB admite UN SOLO ESCRITOR a la vez; añadir
hilos no acelera la escritura concurrente (ley de Amdahl); la solución es
particionar los datos".

BLOQUE 3 — PANEL DE DATOS MASIVOS:
Dos casos: (a) caso sísmico (propagación de ondas, octree/mallas); (b) caso de
APUESTAS (registros pequeños y numerosos: jugadas de lotería por fecha,
animalito, taquilla; ideal para árboles B+ y consultas por rango).

BLOQUE 4 — RESULTADOS MEDIDOS (datos reales):
Panel con cifras verificadas: "Escritura ordenada vs aleatoria: x8,6 de
rendimiento y x3,4 de espacio"; "Lookup en caché fría: x267 más lento que en
tibia"; "83.251 fallos de página mayores medidos, frente a 0 en tibia";
"Dataset de 100 millones de jugadas (15,5 GB) validado 149/149"; "Árbol B+ de
4 niveles para 100 M de claves".

Estilo: diagramas de bloques claros, iconos de hardware (CPU, MMU, disco),
paleta sobria (azul/verde/gris), tipografía sans-serif, texto conciso. Formato
vertical apto para material de estudio universitario.
```

---

**Nota:** la propuesta no modifica el recurso original; lo complementa con la
restricción del escritor único, la regla del big-endian y un panel de resultados
medidos, que son los tres aspectos que un estudiante necesita además de la
explicación teórica para implementar el Proyecto 13.
