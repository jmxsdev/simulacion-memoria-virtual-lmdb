# Simulación de Memoria Virtual con LMDB — Fase 1

Proyecto académico (arquitectura del computador): usar LMDB (una base de datos
clave-valor mapeada a memoria) para manejar grandes volúmenes de datos de
apuestas como si fuera memoria secundaria, y luego optimizar las consultas y
escrituras mientras medimos tiempo de acceso y latencia.

El dataset es sintético pero realista en el dominio: jugadas de lotería
venezolana (animalitos, terminales, tripletas) que registran las taquillas a lo
largo de los días, más los resultados de los sorteos a los que se juega.

## Estructura

- `src/types.h` — constantes del dominio y estructuras en memoria.
- `src/keys.h` — codificación binaria de claves y valores. Las claves van en
  BIG-endian porque LMDB compara claves con `memcmp`: el big-endian hace que el
  orden lexicográfico coincida con el orden numérico, que es de lo que dependen
  los barridos por rango.
- `src/generator.cpp` — generador determinista del dataset.

## Sub-bases LMDB

| Base | Para qué sirve | Clave | Valor |
| --- | --- | --- | --- |
| `bets` | registro maestro de la jugada (claves ascendentes → `MDB_APPEND`) | 13 B: lotería, ts, taquilla, seq | 16 B: tipo, selecciones, monto |
| `i_animalito` | consultas de frecuencia y rachas | 16 B: lotería, animalito, ts, taquilla, seq | 8 B: monto |
| `i_terminal` | frecuencia de terminales (00–99) | 16 B: lotería, terminal, ts, taquilla, seq | 8 B: monto |
| `i_taquilla` | replay/sincronización por punto de venta | 16 B: taquilla, ts, lotería, seq | 8 B: monto |
| `draws` | ganadores por lotería/día/sorteo | 6 B: lotería, día, sorteo | 2 B: animalito, número |

## Compilar y correr

```sh
make            # compila las tres herramientas en bin/
make smoke      # genera el dataset de humo de 1M de jugadas en data/smoke.lmdb
make smoke-clean  # borra solo los artefactos de la prueba de humo
make clean      # borra solo los binarios — jamás los datasets
```

## Herramientas

| Herramienta | Fase | Qué hace |
| --- | --- | --- |
| `bin/generator` | 1 | genera el dataset determinista (`--bets`, `--years`, `--taquillas`, `--seed`...) |
| `bin/bench` | 2, 4 | benchmarks de escritura y lectura (escritura: `write-append`/`write-random`; lectura: `read-lookup`/`read-scan`/`read-index` con `--cold`) |
| `bin/query` | 3 | consultas de solo lectura y validación contra el manifiesto |

Ejemplos:

```sh
# Benchmark de escritura: 1M registros, lotes de 50k
./bin/bench write-append --records 1000000 --batch 50000 --out data/bench_append.lmdb
./bin/bench write-random  --records 1000000 --batch 50000 --out data/bench_random.lmdb

# Consultas sobre el dataset generado
./bin/query --db data/smoke.lmdb info
./bin/query --db data/smoke.lmdb lookup --lottery 0 --ts 1577838370 --taquilla 33 --seq 0
./bin/query --db data/smoke.lmdb range --lottery 0 --from 1577836800 --to 1578441600
./bin/query --db data/smoke.lmdb animalito-stats --lottery 0
./bin/query --db data/smoke.lmdb racha --lottery 0 --animal 5
./bin/query --db data.smoke.lmdb taquilla --taquilla 7
./bin/query --db data/smoke.lmdb validate   # 149 comprobaciones contra el manifiesto

# Benchmark de lectura: lookups puntuales (tibio y frío)
./bin/bench read-lookup --db data/smoke.lmdb --n 200000          # tibio
./bin/bench read-lookup --db data/smoke.lmdb --n 200000 --cold   # frío

# Benchmark de lectura: barrido secuencial por lotería
./bin/bench read-scan --db data/smoke.lmdb --lottery 0           # tibio
./bin/bench read-scan --db data/smoke.lmdb --lottery 0 --cold    # frío

# Benchmark de lectura: barrido del índice i_animalito
./bin/bench read-index --db data/smoke.lmdb                      # tibio
./bin/bench read-index --db data/smoke.lmdb --cold               # frío
```

### Resultados medidos (Fase 2)

1 M de registros de 104 B, lotes de 50 k, misma máquina:

| Métrica | Append (ordenado) | Aleatorio |
| --- | --- | --- |
| Rendimiento | 1,94 M registros/s · 192 MB/s | 226 k registros/s · 22,4 MB/s |
| Commit p50 | 11,6 ms | 101 ms |
| Tamaño final | 118 MB | 398 MB |

Ordenar las claves vale $\times$8,6 de rendimiento y $\times$3,4 de espacio.

### Resultados medidos (Fase 4: latencia y fallos de página)

Lecturas sobre el dataset de 1M de jugadas ($\approx 180$ MB):

| Modo | Tibio ops/s | Frío ops/s | Factor | Frío majflt | Frío p99 |
| --- | --- | --- | --- | --- | --- |
| read-lookup (200k) | 1.156.332 | 314.544 | ×3,7 | 68 | 4,3 µs |
| read-scan (167k) | 8.428.337 | 1.150.678 | ×7,3 | 11 | 0,078 µs |
| read-index (900k) | 1.866.943 | 1.499.141 | ×1,2 | 76 | 0,072 µs |

Caché fría liberada con `madvise(MADV_DONTNEED)` sobre la región mmap del
archivo LMDB (sin privilegios de root). Fallos de página medidos con
`/proc/self/stat`.

## Parámetros del generador

| Parámetro | Por defecto | Qué hace |
| --- | --- | --- |
| `--bets` | 1000000 | total de jugadas a generar |
| `--years` | 3 | años de historia |
| `--taquillas` | 50 | cantidad de taquillas (puntos de venta) |
| `--seed` | 42 | semilla del PRNG (mismos parámetros + semilla ⇒ salida idéntica) |
| `--out` | data/lottery.lmdb | archivo LMDB de salida (archivo único, `MDB_NOSUBDIR`) |
| `--map-gb` | 8 | tope del mapa de memoria en GiB |
| `--batch-txn` | 50000 | jugadas por transacción de escritura |

El generador se niega a pisar un archivo de salida que ya exista, así que dos
datasets jamás se mezclan en silencio. Junto al dataset se escribe
`<nombre>.manifest.json` con los conteos exactos, las frecuencias por
animalito y por terminal, las estadísticas del B+tree de cada sub-base y el
tamaño del archivo — la verdad absoluta contra la que las fases siguientes
validarán sus consultas.

## Resultados medidos de la prueba de humo (1M de jugadas)

- 1,000,000 jugadas exactas; validación de agregados: **APROBADA**.
- 37,230 sorteos (6 loterías × 3 años).
- Tamaño en disco: 180 MB → **≈180 B por jugada** incluyendo el B+tree.
  Regla para dimensionar: 10M ≈ 1.8 GB · 50M ≈ 9 GB · **100M ≈ 18 GB** ·
  500M ≈ 90 GB.
- Auto-prueba de orden de claves big-endian: **APROBADA**.
- Determinismo verificado: al regenerar con la misma semilla, el archivo
  salió byte a byte idéntico (188,751,872 B).

## Plan de fases

1. **Setup + generador** — listo.
2. **Benchmarks de escritura** — listo.
3. **Capa de consultas + validación** — listo (149/149).
4. **Latencia y fallos de página** — listo (tibio vs. frío con madvise).
5. **Optimizaciones** — listo (madvise hints + dataset 10M).
6. **Informe final** — listo.
