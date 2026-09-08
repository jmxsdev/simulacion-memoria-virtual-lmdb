/// @file bench.cpp
/// @brief Benchmarks de escritura y lectura sobre LMDB (Fases 2 y 4).
///
/// Modos de escritura (Fase 2):
///   write-append: escribe registros con claves estrictamente ascendentes
///                 usando MDB_APPEND — el camino rápido del B+tree, que es
///                 el que aprovecha el generador del dataset.
///   write-random: escribe las mismas cantidades con claves en orden
///                 aleatorio y puts normales — el camino lento, incluido
///                 para que la comparación demuestre POR QUÉ ordenar las
///                 claves importa.
///
/// Modos de lectura (Fase 4):
///   read-lookup: N búsquedas puntuales sobre claves existentes muestreadas
///                del propio dataset.
///   read-scan:   barrido secuencial completo de la lotería indicada.
///   read-index:  barrido secuencial completo del índice i_animalito.
///   --cold:      libera el caché de páginas ANTES de medir (madvise +
///                posix_fadvise con DONTNEED, sin privilegios de root).
///                Sin --cold, la corrida es "tibia" (páginas en RAM).
///
/// Además de duración y percentiles, cada corrida reporta el incremento de
/// fallos de página MENORES y MAYORES medido en /proc/self/stat: la
/// diferencia tibio/frío ES el costo de la paginación por demanda.

#include <lmdb.h>

#include <fcntl.h>
#include <sys/mman.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "env.h"
#include "keys.h"

namespace fs = std::filesystem;
using namespace lotto;

namespace {

/// @brief Tamaño del valor sintético (96 B), para modelar un registro típico.
constexpr size_t kValueSize = 96;
/// @brief Tamaño de la clave sintética (8 B: contador big-endian).
constexpr size_t kKeySize = 8;

/// @brief Calcula un percentil sobre el vector ya ordenado.
/// @param[in] values Valores de duración (deben estar ordenados).
/// @param[in] p      Percentil en [0, 100].
/// @return Valor del percentil, o 0 si el vector está vacío.
double percentile(const std::vector<double>& values, double p) {
  if (values.empty()) return 0.0;
  size_t idx = static_cast<size_t>(values.size() * p / 100.0);
  if (idx >= values.size()) idx = values.size() - 1;
  return values[idx];
}

/// @brief Ejecuta un benchmark de escritura por lotes.
///
/// @param[in] out      Archivo LMDB de salida (debe no existir, salvo --force).
/// @param[in] records  Total de registros a escribir.
/// @param[in] batch    Registros por transacción de escritura.
/// @param[in] append   true: claves ascendentes + MDB_APPEND; false: claves
///                     aleatorias + put normal.
/// @param[in] map_gb   Tope del mapa de memoria en GiB.
/// @param[in] seed     Semilla para el modo aleatorio.
/// @throws std::runtime_error ante cualquier error de LMDB.
void run_write_bench(const std::string& out, uint64_t records, uint64_t batch,
                     bool append, uint64_t map_gb, uint64_t seed) {
  MDB_env* env = nullptr;
  LMDB_CHECK(mdb_env_create(&env));
  LMDB_CHECK(mdb_env_set_maxdbs(env, 2));  // base principal + "k"
  LMDB_CHECK(mdb_env_set_mapsize(env, map_gb * 1024ULL * 1024ULL * 1024ULL));
  LMDB_CHECK(mdb_env_open(env, out.c_str(), MDB_NOSUBDIR, 0664));

  MDB_dbi dbi{};
  MDB_txn* txn = nullptr;
  LMDB_CHECK(mdb_txn_begin(env, nullptr, 0, &txn));
  try {
    LMDB_CHECK(mdb_dbi_open(txn, "k", MDB_CREATE, &dbi));
  } catch (...) {
    mdb_txn_abort(txn);
    throw;
  }

  SplitMix64 rng(seed);
  std::vector<double> commit_ms;
  commit_ms.reserve((records + batch - 1) / batch);

  auto t0 = std::chrono::steady_clock::now();
  uint64_t in_txn = 0;
  for (uint64_t i = 0; i < records; ++i) {
    std::array<uint8_t, kKeySize> key{};
    std::array<uint8_t, kValueSize> val{};
    if (append) {
      keys::put_be64(key.data(), i);  // clave ascendente
    } else {
      keys::put_be64(key.data(), rng.next());  // clave aleatoria
    }
    for (size_t b = 0; b < val.size(); ++b)
      val[b] = static_cast<uint8_t>((i * 7 + b) & 0xFF);

    MDB_val k{key.size(), key.data()};
    MDB_val v{val.size(), val.data()};
    LMDB_CHECK(mdb_put(txn, dbi, &k, &v, append ? MDB_APPEND : 0));
    ++in_txn;

    if (in_txn >= batch) {
      auto c0 = std::chrono::steady_clock::now();
      LMDB_CHECK(mdb_txn_commit(txn));
      commit_ms.push_back(std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - c0)
                              .count());
      LMDB_CHECK(mdb_txn_begin(env, nullptr, 0, &txn));
      in_txn = 0;
    }
  }
  if (in_txn > 0) {
    auto c0 = std::chrono::steady_clock::now();
    LMDB_CHECK(mdb_txn_commit(txn));
    commit_ms.push_back(std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - c0)
                            .count());
  }

  auto t1 = std::chrono::steady_clock::now();
  const double secs = std::chrono::duration<double>(t1 - t0).count();
  const uint64_t bytes = records * (kKeySize + kValueSize);
  const uint64_t file_bytes = static_cast<uint64_t>(fs::file_size(out));

  std::sort(commit_ms.begin(), commit_ms.end());
  const char* modo = append ? "append (claves ascendentes + MDB_APPEND)"
                            : "aleatorio (puts normales)";

  std::cout << "Benchmark de escritura — modo " << modo << "\n";
  std::cout << "registros=" << records << " lote=" << batch << "\n";
  std::cout << "duracion_ms=" << static_cast<uint64_t>(secs * 1000.0) << "\n";
  std::cout << "rendimiento="
            << static_cast<uint64_t>(static_cast<double>(records) / secs)
            << " registros/s, "
            << static_cast<double>(bytes) / secs / (1024.0 * 1024.0)
            << " MB/s\n";
  std::cout << "commit_ms p50=" << percentile(commit_ms, 50)
            << " p95=" << percentile(commit_ms, 95)
            << " p99=" << percentile(commit_ms, 99) << "\n";
  std::cout << "bytes_en_archivo=" << file_bytes << "\n";

  mdb_dbi_close(env, dbi);
  mdb_env_close(env);
}

// ---------------------------------------------------------------------------
// Escritura concurrente: simula N taquillas escribiendo en paralelo.
// ---------------------------------------------------------------------------

/// @brief Resultado individual de un hilo taquilla.
struct TaquillaResult {
  uint64_t registros = 0;
  double duracion_ms = 0;
  double commit_p50_ms = 0;
  double commit_p95_ms = 0;
};

/// @brief Hilo que simula una taquilla escribiendo jugadas.
///
/// Cada taquilla genera `registros` jugadas con su propio ID, en lotes de
/// `batch` registros. LMDB solo permite un escritor a la vez, así que todos
/// los hilos comparten un mutex — el benchmark mide cuánto se degrada el
/// rendimiento con N taquillas compitiendo por el escritor.
///
/// @param[in]  env      Entorno LMDB compartido.
/// @param[in]  taquilla_id ID de la taquilla (0..N-1).
/// @param[in]  registros Total de registros a escribir.
/// @param[in]  batch    Registros por transacción.
/// @param[in]  seed     Semilla base (se mezcla con taquilla_id).
/// @param[in]  mtx      Mutex compartido para serializar escrituras.
/// @param[out] result   Resultado de esta taquilla.
void taquilla_writer(MDB_env* env, uint16_t taquilla_id, uint64_t registros,
                     uint64_t batch, uint64_t seed, std::mutex& mtx,
                     TaquillaResult& result) {
  SplitMix64 rng(seed + taquilla_id);
  std::vector<double> commit_ms;
  commit_ms.reserve((registros + batch - 1) / batch);

  auto t0 = std::chrono::steady_clock::now();
  uint64_t escritos = 0;

  while (escritos < registros) {
    std::lock_guard<std::mutex> lock(mtx);

    MDB_dbi dbi{};
    MDB_txn* txn = nullptr;
    LMDB_CHECK(mdb_txn_begin(env, nullptr, 0, &txn));
    LMDB_CHECK(mdb_dbi_open(txn, "k", MDB_CREATE, &dbi));

    uint64_t en_lote = 0;
    while (en_lote < batch && escritos < registros) {
      // Clave: [lotería u8][ts u64][taquilla u16][seq u16]
      // La lotería se cicla entre 6; el ts crece; la taquilla es fija por hilo.
      const uint8_t lottery = static_cast<uint8_t>(escritos % 6);
      const uint64_t ts = 1577836800ULL + (escritos / 600);
      const uint16_t seq = static_cast<uint16_t>(escritos % 65536);

      std::array<uint8_t, 13> key{};
      key[0] = lottery;
      keys::put_be64(&key[1], ts);
      keys::put_be16(&key[9], taquilla_id);
      keys::put_be16(&key[11], seq);

      // Valor: [tipo u8][selecciones u7][monto u8]
      std::array<uint8_t, 16> val{};
      val[0] = static_cast<uint8_t>(rng.next() % 3);  // tipo
      for (size_t b = 1; b < 8; ++b)
        val[b] = static_cast<uint8_t>(rng.next() & 0xFF);
      val[8] = static_cast<uint8_t>((rng.next() % 10000) & 0xFF);
      val[9] = static_cast<uint8_t>((rng.next() % 10000) >> 8);

      MDB_val k{key.size(), key.data()};
      MDB_val v{val.size(), val.data()};
      LMDB_CHECK(mdb_put(txn, dbi, &k, &v, 0));
      ++escritos;
      ++en_lote;
    }

    auto c0 = std::chrono::steady_clock::now();
    LMDB_CHECK(mdb_txn_commit(txn));
    commit_ms.push_back(std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - c0)
                            .count());
    mdb_dbi_close(env, dbi);
  }

  auto t1 = std::chrono::steady_clock::now();
  result.registros = escritos;
  result.duracion_ms =
      std::chrono::duration<double, std::milli>(t1 - t0).count();
  std::sort(commit_ms.begin(), commit_ms.end());
  result.commit_p50_ms = percentile(commit_ms, 50);
  result.commit_p95_ms = percentile(commit_ms, 95);
}

/// @brief Benchmark de escritura concurrente: N taquillas escribiendo en
///        paralelo sobre un mismo entorno LMDB.
///
/// LMDB solo permite un escritor a la vez (su constraint fundamental). Este
/// benchmark demuestra cuánto se degrada el rendimiento cuando múltiples
/// taquillas compiten por el escritor serializado.
///
/// @param[in] out        Archivo LMDB de salida.
/// @param[in] n_taquillas Cantidad de taquillas (hilos).
/// @param[in] registros  Registros por taquilla.
/// @param[in] batch      Registros por transacción.
/// @param[in] map_gb     Tope del mapa en GiB.
/// @param[in] seed       Semilla base.
void run_write_concurrent(const std::string& out, uint32_t n_taquillas,
                          uint64_t registros, uint64_t batch, uint64_t map_gb,
                          uint64_t seed) {
  MDB_env* env = nullptr;
  LMDB_CHECK(mdb_env_create(&env));
  LMDB_CHECK(mdb_env_set_maxdbs(env, 2));
  LMDB_CHECK(mdb_env_set_mapsize(env, map_gb * 1024ULL * 1024ULL * 1024ULL));
  LMDB_CHECK(mdb_env_set_maxreaders(env, n_taquillas + 1));
  LMDB_CHECK(mdb_env_open(env, out.c_str(), MDB_NOSUBDIR, 0664));

  std::mutex mtx;
  std::vector<TaquillaResult> results(n_taquillas);

  auto t0 = std::chrono::steady_clock::now();

  // Lanzar hilos — cada uno simula una taquilla.
  std::vector<std::thread> threads;
  threads.reserve(n_taquillas);
  for (uint32_t i = 0; i < n_taquillas; ++i) {
    threads.emplace_back(taquilla_writer, env, static_cast<uint16_t>(i),
                         registros, batch, seed, std::ref(mtx),
                         std::ref(results[i]));
  }
  for (auto& t : threads) t.join();

  auto t1 = std::chrono::steady_clock::now();
  const double secs = std::chrono::duration<double>(t1 - t0).count();

  // Agregar resultados.
  uint64_t total_registros = 0;
  std::vector<double> all_commit_p50, all_commit_p95;
  for (const auto& r : results) {
    total_registros += r.registros;
    all_commit_p50.push_back(r.commit_p50_ms);
    all_commit_p95.push_back(r.commit_p95_ms);
  }
  std::sort(all_commit_p50.begin(), all_commit_p50.end());
  std::sort(all_commit_p95.begin(), all_commit_p95.end());

  const uint64_t bytes = total_registros * (13 + 16);
  const uint64_t file_bytes = static_cast<uint64_t>(fs::file_size(out));

  std::cout << "Benchmark de escritura concurrente — " << n_taquillas
            << " taquillas\n";
  std::cout << "registros_por_taquilla=" << registros
            << " total=" << total_registros << " lote=" << batch << "\n";
  std::cout << "duracion_ms=" << static_cast<uint64_t>(secs * 1000.0) << "\n";
  std::cout << "rendimiento="
            << static_cast<uint64_t>(static_cast<double>(total_registros) / secs)
            << " registros/s, "
            << static_cast<double>(bytes) / secs / (1024.0 * 1024.0)
            << " MB/s\n";
  std::cout << "commit_taquilla_ms p50=" << percentile(all_commit_p50, 50)
            << " p95=" << percentile(all_commit_p50, 95) << "\n";
  std::cout << "commit_global_ms p95=" << percentile(all_commit_p95, 95)
            << "\n";
  std::cout << "bytes_en_archivo=" << file_bytes << "\n";

  // Desglose por taquilla.
  for (uint32_t i = 0; i < n_taquillas; ++i) {
    std::cout << "  taquilla_" << i << "=" << results[i].registros
              << " registros en " << static_cast<uint64_t>(results[i].duracion_ms)
              << " ms (commit p50=" << results[i].commit_p50_ms << " ms)\n";
  }

  mdb_env_close(env);
}

// ---------------------------------------------------------------------------
// Fase 4: benchmarks de lectura, fallos de página y caché fría vs. tibia.
// ---------------------------------------------------------------------------

/// @brief Contadores de fallos de página del proceso (minflt/majflt).
struct Fallos {
  uint64_t minflt = 0;  ///< Fallos menores (página ya en caché).
  uint64_t majflt = 0;  ///< Fallos mayores (I/O real al dispositivo).
};

/// @brief Lee los contadores de fallos de página de /proc/self/stat.
///
/// Tras el campo 2 (nombre entre paréntesis), minflt es el token 7 y majflt
/// el 9 (contando desde 0).
/// @return Contadores actuales del proceso.
/// @throws std::runtime_error si no se puede leer /proc/self/stat.
Fallos leer_fallos() {
  std::ifstream in("/proc/self/stat");
  if (!in) throw std::runtime_error("no se pudo leer /proc/self/stat");
  std::string s((std::istreambuf_iterator<char>(in)),
                std::istreambuf_iterator<char>());
  const size_t cierre = s.rfind(')');
  if (cierre == std::string::npos)
    throw std::runtime_error("formato inesperado en /proc/self/stat");
  std::istringstream iss(s.substr(cierre + 2));
  std::string tok;
  Fallos f;
  int idx = 0;
  while (iss >> tok) {
    if (idx == 7) f.minflt = std::stoull(tok);
    if (idx == 9) f.majflt = std::stoull(tok);
    ++idx;
  }
  return f;
}

/// @brief Libera el caché de páginas del dataset SIN privilegios de root.
///
/// Encuentra la región mmap del archivo LMDB en /proc/self/maps y aplica
/// madvise(MADV_DONTNEED) para forzar la re-carga desde disco. También
/// usa posix_fadvise(DONTNEED) como refuerzo sobre el descriptor. La
/// alternativa clásica (echo 3 > /proc/sys/vm/drop_caches) exige root;
/// este método no lo necesita.
///
/// @param[in] env     Entorno abierto del dataset.
/// @param[in] db_path Ruta del archivo LMDB (para buscarlo en /proc/self/maps).
void soltar_cache(MDB_env* env, const std::string& db_path) {
  bool ok = false;

  // 1. Encontrar la región mmap en /proc/self/maps y aplicar madvise.
  std::ifstream maps("/proc/self/maps");
  if (maps) {
    std::string line;
    while (std::getline(maps, line)) {
      if (line.find(db_path) != std::string::npos &&
          line.find("-lock") == std::string::npos) {
        unsigned long start = 0, end = 0;
        if (sscanf(line.c_str(), "%lx-%lx", &start, &end) == 2 && end > start) {
          const size_t len = end - start;
          if (madvise(reinterpret_cast<void*>(start), len, MADV_DONTNEED) == 0)
            ok = true;
        }
        break;
      }
    }
  }

  // 2. posix_fadvise como refuerzo.
  mdb_filehandle_t fd = 0;
  if (mdb_env_get_fd(env, &fd) == MDB_SUCCESS) {
    if (posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED) == 0) ok = true;
  }

  if (!ok)
    std::cout << "aviso: no se pudo liberar el caché de páginas "
                 "(madvise/fadvise)\n";
}

/// @brief Busca la región mmap de un archivo LMDB en /proc/self/maps.
/// @param[in] db_path Ruta del archivo LMDB.
/// @param[out] start  Dirección inicial de la región mmap.
/// @param[out] len    Tamaño de la región en bytes.
/// @return true si se encontró la región.
bool encontrar_mmap(const std::string& db_path, void*& start, size_t& len) {
  std::ifstream maps("/proc/self/maps");
  if (!maps) return false;
  std::string line;
  while (std::getline(maps, line)) {
    if (line.find(db_path) != std::string::npos &&
        line.find("-lock") == std::string::npos) {
      unsigned long s = 0, e = 0;
      if (sscanf(line.c_str(), "%lx-%lx", &s, &e) == 2 && e > s) {
        start = reinterpret_cast<void*>(s);
        len = e - s;
        return true;
      }
      break;
    }
  }
  return false;
}

/// @brief Aplica un hint de madvise sobre la región mmap del dataset.
/// @param[in] db_path Ruta del archivo LMDB.
/// @param[in] advice  Hint: MADV_SEQUENTIAL, MADV_RANDOM, etc.
void aplicar_hint_madvise(const std::string& db_path, int advice) {
  void* addr = nullptr;
  size_t len = 0;
  if (encontrar_mmap(db_path, addr, len)) {
    madvise(addr, len, advice);
  }
}

/// @brief Abre el dataset en solo lectura y deja lista una transacción con
///        las sub-bases abiertas dentro de ella.
/// @param[in]  db_path Ruta del dataset.
/// @param[out] db      Entorno abierto.
/// @param[out] txn     Transacción de lectura activa (abortar al terminar).
void preparar_lectura(const std::string& db_path, DbHandles& db,
                      MDB_txn*& txn) {
  open_readonly(db_path, db);
  LMDB_CHECK(mdb_txn_begin(db.env, nullptr, MDB_RDONLY, &txn));
  abrir_subbases(txn, db);
}

/// @brief Muestra claves existentes de `bets` con zancada uniforme.
///
/// Necesario porque una clave inventada al azar casi nunca existe (el
/// generador produce marcas de tiempo dispersas), y un lookup fallido no
/// ejercita la lectura real de páginas de datos.
///
/// @param[in]  txn Transacción de lectura.
/// @param[in]  db  Manejadores abiertos en @p txn.
/// @param[out] muestra Claves muestreadas (13 bytes cada una).
void muestrear_claves(MDB_txn* txn, const DbHandles& db,
                      std::vector<std::array<uint8_t, 13>>& muestra) {
  constexpr size_t kMaxMuestra = 100000;
  MDB_stat st{};
  LMDB_CHECK(mdb_stat(txn, db.bets, &st));
  const size_t zancada =
      st.ms_entries > kMaxMuestra ? st.ms_entries / kMaxMuestra : 1;
  MDB_cursor* cur = nullptr;
  LMDB_CHECK(mdb_cursor_open(txn, db.bets, &cur));
  MDB_val k{}, v{};
  size_t pos = 0;
  if (mdb_cursor_get(cur, &k, &v, MDB_FIRST) == MDB_SUCCESS) {
    do {
      if (pos % zancada == 0) {
        std::array<uint8_t, 13> clave{};
        std::memcpy(clave.data(), k.mv_data, 13);
        muestra.push_back(clave);
      }
      ++pos;
    } while (mdb_cursor_get(cur, &k, &v, MDB_NEXT) == MDB_SUCCESS);
  }
  mdb_cursor_close(cur);
  if (muestra.empty()) throw std::runtime_error("la base bets está vacía");
}

/// @brief `read-lookup`: N búsquedas puntuales sobre claves existentes.
/// @param[in] db_path Ruta del dataset.
/// @param[in] n       Cantidad de búsquedas.
/// @param[in] cold    true para liberar el caché antes de medir.
/// @param[in] seed    Semilla para elegir las claves.
void correr_read_lookup(const std::string& db_path, uint64_t n, bool cold,
                        uint64_t seed) {
  DbHandles db;
  MDB_txn* txn = nullptr;
  preparar_lectura(db_path, db, txn);

  std::vector<std::array<uint8_t, 13>> muestra;
  muestrear_claves(txn, db, muestra);

  if (cold) soltar_cache(db.env, db_path);
  // Hint al kernel: acceso aleatorio → desactiva relectura anticipada.
  aplicar_hint_madvise(db_path, MADV_RANDOM);
  const Fallos antes = leer_fallos();

  SplitMix64 rng(seed);
  std::vector<double> lat_us;
  lat_us.reserve(n);
  uint64_t aciertos = 0;
  const auto t0 = std::chrono::steady_clock::now();
  for (uint64_t i = 0; i < n; ++i) {
    const auto& karr = muestra[rng.next() % muestra.size()];
    MDB_val k{karr.size(), const_cast<uint8_t*>(karr.data())};
    MDB_val v{};
    const auto s = std::chrono::steady_clock::now();
    const int rc = mdb_get(txn, db.bets, &k, &v);
    lat_us.push_back(
        std::chrono::duration<double, std::micro>(
            std::chrono::steady_clock::now() - s)
            .count());
    if (rc == MDB_SUCCESS) ++aciertos;
  }
  const auto t1 = std::chrono::steady_clock::now();
  const double secs = std::chrono::duration<double>(t1 - t0).count();
  const Fallos despues = leer_fallos();

  std::sort(lat_us.begin(), lat_us.end());
  std::cout << "Benchmark de lectura — lookups puntuales (modo "
            << (cold ? "frío" : "tibio") << ")\n";
  std::cout << "operaciones=" << n << " aciertos=" << aciertos << "\n";
  std::cout << "ops/s=" << static_cast<uint64_t>(static_cast<double>(n) / secs)
            << "\n";
  std::cout << "latencia_us p50=" << percentile(lat_us, 50)
            << " p95=" << percentile(lat_us, 95)
            << " p99=" << percentile(lat_us, 99) << "\n";
  std::cout << "fallos_pagina_menores=" << (despues.minflt - antes.minflt)
            << " mayores=" << (despues.majflt - antes.majflt) << "\n";

  mdb_txn_abort(txn);
  close_env(db);
}

/// @brief `read-scan`: barrido secuencial de todas las jugadas de una
///        lotería.
/// @param[in] db_path Ruta del dataset.
/// @param[in] lottery Lotería a barrer.
/// @param[in] cold    true para liberar el caché antes de medir.
void correr_read_scan(const std::string& db_path, uint8_t lottery, bool cold) {
  DbHandles db;
  MDB_txn* txn = nullptr;
  preparar_lectura(db_path, db, txn);
  if (cold) soltar_cache(db.env, db_path);
  // Hint al kernel: acceso secuencial → amplía la relectura anticipada.
  aplicar_hint_madvise(db_path, MADV_SEQUENTIAL);
  const Fallos antes = leer_fallos();

  MDB_cursor* cur = nullptr;
  LMDB_CHECK(mdb_cursor_open(txn, db.bets, &cur));
  auto clave0 = keys::encode_bets_key(lottery, 0, 0, 0);
  MDB_val k{clave0.size(), clave0.data()};
  MDB_val v{};
  uint64_t count = 0;
  std::vector<double> lat_us;
  const auto t0 = std::chrono::steady_clock::now();
  if (mdb_cursor_get(cur, &k, &v, MDB_SET_RANGE) == MDB_SUCCESS) {
    do {
      const auto s = std::chrono::steady_clock::now();
      keys::BetsKey bk = keys::decode_bets_key(
          static_cast<const uint8_t*>(k.mv_data), k.mv_size);
      lat_us.push_back(
          std::chrono::duration<double, std::micro>(
              std::chrono::steady_clock::now() - s)
              .count());
      if (bk.lottery != lottery) break;
      ++count;
    } while (mdb_cursor_get(cur, &k, &v, MDB_NEXT) == MDB_SUCCESS);
  }
  const auto t1 = std::chrono::steady_clock::now();
  const double secs = std::chrono::duration<double>(t1 - t0).count();
  const Fallos despues = leer_fallos();
  mdb_cursor_close(cur);

  std::sort(lat_us.begin(), lat_us.end());
  std::cout << "Benchmark de lectura — barrido secuencial lotería "
            << (int)lottery << " (modo " << (cold ? "frío" : "tibio") << ")\n";
  std::cout << "jugadas=" << count << "\n";
  std::cout << "ops/s=" << static_cast<uint64_t>(static_cast<double>(count) / secs)
            << "\n";
  std::cout << "paso_us p50=" << percentile(lat_us, 50)
            << " p95=" << percentile(lat_us, 95)
            << " p99=" << percentile(lat_us, 99) << "\n";
  std::cout << "fallos_pagina_menores=" << (despues.minflt - antes.minflt)
            << " mayores=" << (despues.majflt - antes.majflt) << "\n";

  mdb_txn_abort(txn);
  close_env(db);
}

/// @brief `read-index`: barrido secuencial completo del índice i_animalito.
/// @param[in] db_path Ruta del dataset.
/// @param[in] cold    true para liberar el caché antes de medir.
void correr_read_index(const std::string& db_path, bool cold) {
  DbHandles db;
  MDB_txn* txn = nullptr;
  preparar_lectura(db_path, db, txn);
  if (cold) soltar_cache(db.env, db_path);
  aplicar_hint_madvise(db_path, MADV_SEQUENTIAL);
  const Fallos antes = leer_fallos();

  MDB_cursor* cur = nullptr;
  LMDB_CHECK(mdb_cursor_open(txn, db.i_animalito, &cur));
  MDB_val k{}, v{};
  uint64_t count = 0;
  std::vector<double> lat_us;
  const auto t0 = std::chrono::steady_clock::now();
  if (mdb_cursor_get(cur, &k, &v, MDB_FIRST) == MDB_SUCCESS) {
    do {
      const auto s = std::chrono::steady_clock::now();
      lat_us.push_back(
          std::chrono::duration<double, std::micro>(
              std::chrono::steady_clock::now() - s)
              .count());
      ++count;
    } while (mdb_cursor_get(cur, &k, &v, MDB_NEXT) == MDB_SUCCESS);
  }
  const auto t1 = std::chrono::steady_clock::now();
  const double secs = std::chrono::duration<double>(t1 - t0).count();
  const Fallos despues = leer_fallos();
  mdb_cursor_close(cur);

  std::sort(lat_us.begin(), lat_us.end());
  std::cout << "Benchmark de lectura — índice i_animalito completo (modo "
            << (cold ? "frío" : "tibio") << ")\n";
  std::cout << "entradas=" << count << "\n";
  std::cout << "ops/s=" << static_cast<uint64_t>(static_cast<double>(count) / secs)
            << "\n";
  std::cout << "paso_us p50=" << percentile(lat_us, 50)
            << " p95=" << percentile(lat_us, 95)
            << " p99=" << percentile(lat_us, 99) << "\n";
  std::cout << "fallos_pagina_menores=" << (despues.minflt - antes.minflt)
            << " mayores=" << (despues.majflt - antes.majflt) << "\n";

  mdb_txn_abort(txn);
  close_env(db);
}

}  // namespace

/// @brief Punto de entrada: interpreta argumentos y despacha el benchmark.
int main(int argc, char** argv) {
  try {
    std::string mode;
    std::string out = "data/bench.lmdb";
    std::string db_path;
    uint64_t records = 1000000, batch = 50000, map_gb = 8, seed = 42;
    uint64_t n_lookup = 200000;
    uint32_t n_taquillas = 1;
    int lottery = -1;
    bool force = false, cold = false;

    for (int i = 1; i < argc; ++i) {
      std::string a = argv[i];
      auto val = [&]() -> std::string {
        if (i + 1 >= argc)
          throw std::runtime_error(a + " requiere un valor");
        return argv[++i];
      };
      if (a == "write-append" || a == "write-random" ||
          a == "write-concurrent")
        mode = a;
      else if (a == "read-lookup" || a == "read-scan" || a == "read-index")
        mode = a;
      else if (a == "--out") out = val();
      else if (a == "--db") db_path = val();
      else if (a == "--records") records = std::stoull(val());
      else if (a == "--batch") batch = std::stoull(val());
      else if (a == "--map-gb") map_gb = std::stoull(val());
      else if (a == "--seed") seed = std::stoull(val());
      else if (a == "--n") n_lookup = std::stoull(val());
      else if (a == "--taquillas") n_taquillas = std::stoul(val());
      else if (a == "--lottery") lottery = std::stoi(val());
      else if (a == "--cold") cold = true;
      else if (a == "--force") force = true;
      else if (a == "--help") {
        std::cout
            << "Uso: bench <modo> [opciones]\n\n"
            << "Modos de escritura:\n"
            << "  write-append      claves ascendentes + MDB_APPEND (rápido)\n"
            << "  write-random      claves aleatorias (lento)\n"
            << "  write-concurrent  N taquillas escribiendo en paralelo\n"
            << "\nModos de lectura:\n"
            << "  read-lookup   búsquedas puntuales sobre claves existentes\n"
            << "  read-scan     barrido secuencial de una lotería\n"
            << "  read-index    barrido del índice i_animalito\n"
            << "\nOpciones escritura:\n"
            << "  --out PATH        salida (default: data/bench.lmdb)\n"
            << "  --records N       registros (default: 1000000)\n"
            << "  --batch M         tamaño de lote (default: 50000)\n"
            << "  --map-gb G        tamaño del mapa en GB (default: 8)\n"
            << "  --seed S          semilla (default: 42)\n"
            << "  --taquillas T     taquillas para write-concurrent (default: 1)\n"
            << "\nOpciones lectura:\n"
            << "  --db PATH         dataset a leer (requerido)\n"
            << "  --n N             cantidad de lookups (default: 200000)\n"
            << "  --lottery L       lotería para read-scan (default: 0)\n"
            << "  --cold            liberar caché de páginas antes de medir\n"
            << "\n  --force           borrar salida existente (escritura)\n";
        return 0;
      } else {
        throw std::runtime_error("argumento desconocido: " + a);
      }
    }

    if (mode.empty())
      throw std::runtime_error(
          "indica un modo: write-append, write-random, write-concurrent, "
          "read-lookup, read-scan, read-index");

    // Escritura (Fase 2 y concurrente)
    if (mode == "write-append" || mode == "write-random" ||
        mode == "write-concurrent") {
      if (fs::exists(out)) {
        if (!force)
          throw std::runtime_error(
              "el archivo de salida ya existe: " + out +
              " (usa --force para borrarlo o elige otro --out)");
        fs::remove(out);
        fs::remove(out + "-lock");
      }
      if (mode == "write-concurrent") {
        run_write_concurrent(out, n_taquillas, records, batch, map_gb, seed);
      } else {
        run_write_bench(out, records, batch, mode == "write-append", map_gb,
                        seed);
      }
      return 0;
    }

    // Lectura (Fase 4)
    if (db_path.empty())
      throw std::runtime_error("lectura requiere --db RUTA");

    if (mode == "read-lookup") {
      correr_read_lookup(db_path, n_lookup, cold, seed);
    } else if (mode == "read-scan") {
      if (lottery < 0) lottery = 0;
      correr_read_scan(db_path, static_cast<uint8_t>(lottery), cold);
    } else if (mode == "read-index") {
      correr_read_index(db_path, cold);
    }
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
}
