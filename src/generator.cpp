/// @file generator.cpp
/// @brief Generador determinista del dataset sintético de jugadas de lotería.
///
/// Produce un único archivo de entorno LMDB con cinco sub-bases con nombre:
///   bets, i_animalito, i_terminal, i_taquilla, draws (ver types.h / keys.h).
///
/// Determinismo: PRNG splitmix64 sembrado con --seed. Los mismos parámetros y
/// la misma semilla producen siempre un dataset byte a byte idéntico, que es
/// lo que hace reproducibles los benchmarks y la validación de agregados.
///
/// Estrategia de escritura (la decisión de diseño relevante para memoria
/// virtual):
///   - El ciclo EXTERIOR recorre las loterías; las jugadas se generan en
///     orden cronológico dentro de cada lotería, y las de cada hora se
///     amortiguan y se ordenan por marca de tiempo antes de escribirse.
///     Como la clave de `bets` empieza con el id de lotería y sigue con la
///     marca big-endian, el flujo de claves resultante es estrictamente
///     ascendente, lo que permite usar MDB_APPEND en la base maestra
///     (inserción masiva en el borde derecho del B+tree).
///   - Las claves de los índices secundarios se agrupan primero por
///     animalito / terminal / taquilla, así que NO quedan globalmente
///     ordenadas: se insertan con puts normales.
///   - Cada --batch-txn jugadas se cierra la transacción de escritura;
///     transacciones grandes amortizan el costo del fsync por commit.
///
/// Semántica de los índices (documentada para las consultas futuras):
///   - i_animalito guarda una entrada por cada animal involucrado:
///       ANIMALITO -> 1 entrada, TERMINAL -> 0 entradas, TRIPLETA -> 3
///     (cada una con el monto completo; así las consultas de frecuencia
///     cuentan "jugadas donde aparece el animal X").
///   - i_terminal guarda una entrada solo para jugadas tipo TERMINAL.
///   - i_taquilla guarda una entrada por jugada (replay completo por punto
///     de venta).

#include <lmdb.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "keys.h"
#include "types.h"

namespace fs = std::filesystem;
using namespace lotto;

namespace {

// ---------------------------------------------------------------------------
// splitmix64: PRNG pequeño, rápido y totalmente determinista.
// ---------------------------------------------------------------------------

/// @brief PRNG determinista (splitmix64).
///
/// La misma semilla produce siempre la misma secuencia — la propiedad que
/// hace reproducible el dataset bit a bit.
class SplitMix64 {
 public:
  /// @brief Construye el generador a partir de una semilla.
  /// @param[in] seed Semilla arbitraria de 64 bits.
  explicit SplitMix64(uint64_t seed) : state_(seed) {}
  /// @brief Produce el siguiente entero crudo de 64 bits.
  /// @return Valor pseudoaleatorio de 64 bits (uniforme en todo el rango).
  uint64_t next() {
    state_ += 0x9E3779B97F15ULL;
    uint64_t z = state_;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
  }
  /// @brief Produce un double uniforme en [0, 1).
  /// @return Muestra uniforme en [0, 1).
  double next_unit() {
    return static_cast<double>(next() >> 11) / 9007199254740992.0;  // 2^53
  }

 private:
  uint64_t state_;
};

// ---------------------------------------------------------------------------
// Selección ponderada con tabla de pesos acumulados + búsqueda binaria.
// ---------------------------------------------------------------------------

/// @brief Selector de índices por pesos: cada candidato sale con probabilidad
///        proporcional a su peso.
class WeightedPicker {
 public:
  /// @brief Construye la tabla acumulada de pesos.
  /// @param[in] weights Pesos positivos, uno por candidato.
  explicit WeightedPicker(const std::vector<double>& weights) {
    cum_.reserve(weights.size());
    double total = 0.0;
    for (double w : weights) {
      total += w;
      cum_.push_back(total);
    }
  }
  /// @brief Elige un índice en [0, n) a partir de una muestra uniforme.
  /// @param[in] u Muestra uniforme en [0, 1).
  /// @return Índice elegido según los pesos.
  size_t pick(double u) const {
    auto it = std::upper_bound(cum_.begin(), cum_.end(), u * cum_.back());
    size_t idx = static_cast<size_t>(it - cum_.begin());
    return idx < cum_.size() ? idx : cum_.size() - 1;
  }

 private:
  std::vector<double> cum_;
};

// Pesos de demanda por hora del día (00h .. 23h): madrugada floja, repunte al
// mediodía y pico fuerte en la tarde-noche, cerca de los últimos sorteos.
const double kHourWeights[24] = {
    0.30, 0.25, 0.20, 0.20, 0.25, 0.40, 0.60, 0.80,  // 00..07
    1.00, 1.20, 1.40, 1.50, 1.60, 1.50, 1.40, 1.30,  // 08..15
    1.30, 1.50, 1.80, 2.00, 2.00, 1.80, 1.20, 0.60   // 16..23
};
constexpr double kWeekendFactor = 1.30;

// Mezcla de tipos de jugada: 75% animalito, 20% terminal, 5% tripleta.
constexpr double kPAnimalito = 0.75;
constexpr double kPTerminalCutoff = 0.95;

/// @brief Pesos de popularidad tipo Zipf para los animalitos.
///
/// Los animalitos de índice bajo se juegan más, de modo que las consultas de
/// frecuencia tengan estructura real qué encontrar.
/// @return Vector con kAnimalitoCount pesos positivos.
std::vector<double> animalito_weights() {
  std::vector<double> w(static_cast<size_t>(kAnimalitoCount));
  for (size_t i = 0; i < w.size(); ++i)
    w[i] = 1.0 / std::pow(static_cast<double>(i) + 1.0, 0.7);
  return w;
}

/// @brief Horarios de sorteos de cada lotería (horas del día).
struct LotteryConfig {
  std::array<uint8_t, 7> hours;  ///< Horas de los sorteos, en orden.
  uint8_t count;                 ///< Cuántos sorteos al día.
};

const LotteryConfig kLotterySlots[kLotteryCount] = {
    {{10, 12, 14, 16, 18, 20, 0}, 6},  // LOTTO_ACTIVO
    {{9, 11, 13, 15, 17, 19, 0}, 6},   // LA_GRANJITA
    {{8, 10, 12, 14, 16, 18, 20}, 7},  // GUACHARO_ACTIVO
    {{9, 12, 15, 18, 0, 0, 0}, 4},     // SELVA_PLUS
    {{11, 13, 15, 17, 19, 21, 0}, 6},  // LOTERIA_DE_CARACAS
    {{10, 13, 16, 19, 21, 0, 0}, 5},   // LOTTO_REY
};

// ---------------------------------------------------------------------------
// Parámetros de línea de comandos.
// ---------------------------------------------------------------------------

/// @brief Parámetros de línea de comandos del generador.
struct Params {
  uint64_t bets = 1000000;      ///< Total de jugadas a generar.
  uint32_t years = 3;           ///< Años de historia.
  uint32_t taquillas = 50;      ///< Cantidad de taquillas (puntos de venta).
  uint64_t seed = 42;           ///< Semilla del PRNG.
  std::string out = "data/lottery.lmdb";  ///< Archivo LMDB de salida.
  uint64_t map_gb = 8;          ///< Tope del mapa de memoria en GiB.
  uint64_t batch_txn = 50000;   ///< Jugadas por transacción de escritura.
};

/// @brief Interpreta los argumentos de línea de comandos.
/// @param[in] argc Cantidad de argumentos.
/// @param[in] argv Argumentos (sin el nombre del programa).
/// @return Parámetros ya validados.
/// @throws std::runtime_error ante argumentos desconocidos o incompletos.
Params parse_args(int argc, char** argv) {
  Params p;
  auto need_value = [](int argc, char** argv, int& i, const std::string& flag) {
    if (i + 1 >= argc)
      throw std::runtime_error(flag + " requiere un valor");
    return std::string(argv[++i]);
  };
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--bets") p.bets = std::stoull(need_value(argc, argv, i, a));
    else if (a == "--years") p.years = std::stoul(need_value(argc, argv, i, a));
    else if (a == "--taquillas")
      p.taquillas = std::stoul(need_value(argc, argv, i, a));
    else if (a == "--seed") p.seed = std::stoull(need_value(argc, argv, i, a));
    else if (a == "--out") p.out = need_value(argc, argv, i, a);
    else if (a == "--map-gb")
      p.map_gb = std::stoull(need_value(argc, argv, i, a));
    else if (a == "--batch-txn")
      p.batch_txn = std::stoull(need_value(argc, argv, i, a));
    else if (a == "--help") {
      std::cout << "Uso: generator [--bets N] [--years Y] [--taquillas T] "
                   "[--seed S] [--out PATH] [--map-gb G] [--batch-txn M]\n";
      std::exit(0);
    } else {
      throw std::runtime_error("argumento desconocido: " + a);
    }
  }
  return p;
}

// ---------------------------------------------------------------------------
// Ayudante de LMDB: aborta con mensaje claro ante cualquier código de error.
// ---------------------------------------------------------------------------

/// @brief Evalúa @p call y aborta lanzando una excepción clara si falla.
#define LMDB_CHECK(call)                                            \
  do {                                                              \
    int rc_ = (call);                                               \
    if (rc_ != MDB_SUCCESS) {                                       \
      throw std::runtime_error(std::string(#call) + " falló: " +    \
                               mdb_strerror(rc_));                  \
    }                                                               \
  } while (0)

/// @brief Entorno LMDB y los manejadores de las sub-bases abiertas.
struct DbHandles {
  MDB_env* env = nullptr;  ///< Entorno abierto.
  MDB_dbi bets{};          ///< Base maestra de jugadas.
  MDB_dbi i_animalito{};   ///< Índice por animalito.
  MDB_dbi i_terminal{};    ///< Índice por terminal.
  MDB_dbi i_taquilla{};    ///< Índice por taquilla.
  MDB_dbi draws{};         ///< Base de sorteos.
};

/// @brief Crea y abre el entorno LMDB y todas sus sub-bases.
///
/// Abre en modo de archivo único (MDB_NOSUBDIR). El tope del mapa se fija
/// ANTES de abrir el entorno; el archivo crece de forma dispersa y solo
/// ocupa el espacio realmente usado.
///
/// @param[in]  p  Parámetros (ruta de salida, tamaño del mapa).
/// @param[out] db Manejadores que quedan listos para usar.
/// @throws std::runtime_error ante cualquier error de LMDB.
void open_env(const Params& p, DbHandles& db) {
  LMDB_CHECK(mdb_env_create(&db.env));
  LMDB_CHECK(mdb_env_set_maxdbs(db.env, kMaxSubDbs));
  LMDB_CHECK(
      mdb_env_set_mapsize(db.env, p.map_gb * 1024ULL * 1024ULL * 1024ULL));
  // Respaldo de archivo único; permisos 0664, como un archivo de datos típico.
  LMDB_CHECK(mdb_env_open(db.env, p.out.c_str(), MDB_NOSUBDIR, 0664));

  MDB_txn* txn = nullptr;
  LMDB_CHECK(mdb_txn_begin(db.env, nullptr, 0, &txn));
  try {
    LMDB_CHECK(mdb_dbi_open(txn, kDbBets, MDB_CREATE, &db.bets));
    LMDB_CHECK(
        mdb_dbi_open(txn, kDbAnimalitoIndex, MDB_CREATE, &db.i_animalito));
    LMDB_CHECK(mdb_dbi_open(txn, kDbTerminalIndex, MDB_CREATE, &db.i_terminal));
    LMDB_CHECK(mdb_dbi_open(txn, kDbTaquillaIndex, MDB_CREATE, &db.i_taquilla));
    LMDB_CHECK(mdb_dbi_open(txn, kDbDraws, MDB_CREATE, &db.draws));
    LMDB_CHECK(mdb_txn_commit(txn));
  } catch (...) {
    mdb_txn_abort(txn);
    throw;
  }
}

/// @brief Escribe una entrada en una sub-base dentro de la transacción dada.
/// @param[in] txn      Transacción de escritura abierta.
/// @param[in] dbi      Manejador de la sub-base destino.
/// @param[in] key_data Bytes de la clave.
/// @param[in] key_len  Largo de la clave.
/// @param[in] val_data Bytes del valor.
/// @param[in] val_len  Largo del valor.
/// @param[in] append   Si es true usa MDB_APPEND (la clave DEBE ser mayor
///                     que todas las ya escritas en esa sub-base).
/// @throws std::runtime_error ante cualquier error de LMDB.
inline void put(MDB_txn* txn, MDB_dbi dbi, const void* key_data, size_t key_len,
                const void* val_data, size_t val_len, bool append) {
  MDB_val k{key_len, const_cast<void*>(key_data)};
  MDB_val v{val_len, const_cast<void*>(val_data)};
  LMDB_CHECK(mdb_put(txn, dbi, &k, &v, append ? MDB_APPEND : 0));
}

// ---------------------------------------------------------------------------
// Contadores de validación.
// ---------------------------------------------------------------------------

/// @brief Contadores de validación acumulados durante la generación.
struct Counters {
  uint64_t bets = 0;                         ///< Total de jugadas escritas.
  uint64_t per_lottery[kLotteryCount] = {};  ///< Jugadas por lotería.
  uint64_t per_type[3] = {};                 ///< Jugadas por tipo.
  uint64_t per_animalito[kAnimalitoCount] = {};  ///< Jugadas por animalito.
  uint64_t per_terminal[kTerminalCount] = {};    ///< Jugadas por terminal.
  uint64_t draws = 0;                        ///< Total de sorteos escritos.
};

/// @brief Escribe una jugada en la base maestra y en los índices que le tocan.
///
/// Semántica de los índices: ver el bloque @file al inicio del archivo.
///
/// @param[in]    db  Manejadores de las sub-bases.
/// @param[in]    txn Transacción de escritura abierta.
/// @param[in]    b   Jugada a escribir.
/// @param[in,out] c  Contadores de validación que se actualizan.
/// @throws std::runtime_error ante cualquier error de LMDB.
void write_bet(DbHandles& db, MDB_txn* txn, const Bet& b, Counters& c) {
  uint8_t type = static_cast<uint8_t>(b.type);
  auto key = keys::encode_bets_key(b.lottery, b.ts, b.taquilla, b.seq);
  auto val = keys::encode_bets_value(type, b.sel0, b.sel1, b.sel2, b.amount_cents);
  put(txn, db.bets, key.data(), key.size(), val.data(), val.size(),
      /*append=*/true);

  auto iv = keys::encode_index_value(b.amount_cents);

  // Índice por animalito: una entrada por cada animal involucrado.
  if (b.type == BetType::Animalito) {
    auto ia = keys::encode_index_key(b.lottery, b.sel0, b.ts, b.taquilla, b.seq);
    put(txn, db.i_animalito, ia.data(), ia.size(), iv.data(), iv.size(), false);
  } else if (b.type == BetType::Tripleta) {
    const uint8_t sels[3] = {b.sel0, b.sel1, b.sel2};
    for (uint8_t s : sels) {
      auto ia = keys::encode_index_key(b.lottery, s, b.ts, b.taquilla, b.seq);
      put(txn, db.i_animalito, ia.data(), ia.size(), iv.data(), iv.size(), false);
    }
  }

  // Índice por terminal: solo jugadas tipo TERMINAL.
  if (b.type == BetType::Terminal) {
    auto it = keys::encode_index_key(b.lottery, b.sel0, b.ts, b.taquilla, b.seq);
    put(txn, db.i_terminal, it.data(), it.size(), iv.data(), iv.size(), false);
  }

  // Índice por taquilla: una entrada por jugada (replay completo por punto de venta).
  auto iq = keys::encode_taquilla_key(b.taquilla, b.ts, b.lottery, b.seq);
  put(txn, db.i_taquilla, iq.data(), iq.size(), iv.data(), iv.size(), false);

  c.bets++;
  c.per_lottery[b.lottery]++;
  c.per_type[type]++;
  if (b.type == BetType::Animalito) c.per_animalito[b.sel0]++;
  if (b.type == BetType::Tripleta) {
    c.per_animalito[b.sel0]++;
    c.per_animalito[b.sel1]++;
    c.per_animalito[b.sel2]++;
  }
  if (b.type == BetType::Terminal) c.per_terminal[b.sel0]++;
}

/// @brief Arma el tipo, las selecciones y el monto de una jugada (todo
///        determinista a partir del PRNG).
/// @param[in] rng     Generador pseudoaleatorio sembrado.
/// @param[in] animals Selector ponderado de animalitos (popularidad Zipf).
/// @param[in] lottery Id de la lotería.
/// @param[in] ts      Marca de tiempo (segundos desde la época).
/// @param[in] taquilla Id de la taquilla.
/// @param[in] seq     Desambiguador dentro de (ts, taquilla).
/// @return Jugada lista para escribir.
Bet make_bet(SplitMix64& rng, const WeightedPicker& animals, uint8_t lottery,
             uint64_t ts, uint16_t taquilla, uint16_t seq) {
  Bet b;
  b.lottery = lottery;
  b.ts = ts;
  b.taquilla = taquilla;
  b.seq = seq;

  double u = rng.next_unit();
  if (u < kPAnimalito) {
    b.type = BetType::Animalito;
    b.sel0 = static_cast<uint8_t>(animals.pick(rng.next_unit()));
    b.sel1 = 0;
    b.sel2 = 0;
  } else if (u < kPTerminalCutoff) {
    b.type = BetType::Terminal;
    b.sel0 = static_cast<uint8_t>(rng.next() % kTerminalCount);
    b.sel1 = 0;
    b.sel2 = 0;
  } else {
    b.type = BetType::Tripleta;
    // Three distinct animalitos stored ascending (order-insensitive play).
    uint8_t s[3];
    for (int k = 0; k < 3; ++k) {
      uint8_t cand;
      do {
        cand = static_cast<uint8_t>(animals.pick(rng.next_unit()));
      } while (cand == s[0] || (k > 1 && cand == s[1]));
      s[k] = cand;
    }
    std::sort(s, s + 3);
    b.sel0 = s[0];
    b.sel1 = s[1];
    b.sel2 = s[2];
  }

  // Monto log-uniforme en céntimos: 100 .. ~100000 céntimos.
  uint64_t base = 100ULL << (rng.next() % 10);
  b.amount_cents = base + (rng.next() % base);
  return b;
}

/// @brief Auto-prueba: verifica el orden big-endian de las claves caminando
///        con un cursor.
///
/// Lee las primeras jugadas de `bets` y comprueba que la marca de tiempo
/// nunca desciende dentro de la misma lotería. Si la codificación big-endian
/// estuviera rota, los barridos por rango de fecha fallarían en silencio.
///
/// @param[in] db Manejadores del entorno ya escrito.
/// @throws std::runtime_error si el orden no es ascendente.
void self_test_key_order(const DbHandles& db) {
  MDB_txn* txn = nullptr;
  LMDB_CHECK(mdb_txn_begin(db.env, nullptr, MDB_RDONLY, &txn));
  MDB_cursor* cur = nullptr;
  LMDB_CHECK(mdb_cursor_open(txn, db.bets, &cur));
  MDB_val k{}, v{};
  int rc = mdb_cursor_get(cur, &k, &v, MDB_FIRST);
  if (rc != MDB_SUCCESS)
    throw std::runtime_error("auto-prueba: no se pudo leer la primera jugada");
  keys::BetsKey prev =
      keys::decode_bets_key(static_cast<const uint8_t*>(k.mv_data), k.mv_size);
  for (int i = 0; i < 4; ++i) {
    rc = mdb_cursor_get(cur, &k, &v, MDB_NEXT);
    if (rc != MDB_SUCCESS)
      throw std::runtime_error("auto-prueba: no se pudo leer la siguiente jugada");
    keys::BetsKey cur_key = keys::decode_bets_key(
        static_cast<const uint8_t*>(k.mv_data), k.mv_size);
    if (cur_key.lottery != prev.lottery) break;  // pasó a la siguiente lotería
    if (cur_key.ts < prev.ts)
      throw std::runtime_error(
          "auto-prueba FALLÓ: las marcas de tiempo no ascienden en el orden "
          "de claves (la codificación big-endian está rota)");
    prev = cur_key;
  }
  mdb_cursor_close(cur);
  mdb_txn_abort(txn);
  std::cout << "AUTO-PRUEBA DE ORDEN DE CLAVES: APROBADA "
               "(las marcas big-endian ascienden)\n";
}

// ---------------------------------------------------------------------------
// Estadísticas y manifiesto.
// ---------------------------------------------------------------------------

/// @brief Reúne estadísticas de cada sub-base y escribe el manifiesto JSON
///        con la verdad absoluta del dataset generado.
///
/// El manifiesto guarda: parámetros usados, totales por lotería/tipo/
/// animalito/terminal, estadísticas de páginas del B+tree por sub-base,
/// tamaño del archivo, duración y el resultado de la validación.
///
/// @param[in] p             Parámetros usados en la corrida.
/// @param[in] c             Contadores acumulados.
/// @param[in] db            Manejadores del entorno.
/// @param[in] file_bytes    Tamaño real del archivo LMDB en bytes.
/// @param[in] seconds       Duración total de la generación.
/// @param[in] manifest_path Ruta donde escribir el JSON.
/// @throws std::runtime_error ante cualquier error de LMDB.
void write_manifest(const Params& p, const Counters& c, const DbHandles& db,
                    uint64_t file_bytes, double seconds,
                    const std::string& manifest_path) {
  MDB_txn* txn = nullptr;
  LMDB_CHECK(mdb_txn_begin(db.env, nullptr, MDB_RDONLY, &txn));
  auto stat_db = [&](MDB_dbi dbi) {
    MDB_stat st{};
    LMDB_CHECK(mdb_stat(txn, dbi, &st));
    return st;
  };
  MDB_stat s_bets = stat_db(db.bets);
  MDB_stat s_ani = stat_db(db.i_animalito);
  MDB_stat s_ter = stat_db(db.i_terminal);
  MDB_stat s_taq = stat_db(db.i_taquilla);
  MDB_stat s_drw = stat_db(db.draws);
  mdb_txn_abort(txn);  // las transacciones de lectura se cierran con abort

  bool pass = (c.bets == p.bets);
  uint64_t lot_sum = 0;
  for (int l = 0; l < kLotteryCount; ++l) lot_sum += c.per_lottery[l];
  pass = pass && (lot_sum == p.bets);

  std::ofstream out(manifest_path);
  out << "{\n";
  out << "  \"params\": {\"bets\": " << p.bets << ", \"years\": " << p.years
      << ", \"taquillas\": " << p.taquillas << ", \"seed\": " << p.seed
      << ", \"map_gb\": " << p.map_gb << ", \"batch_txn\": " << p.batch_txn
      << "},\n";
  out << "  \"bets_total\": " << c.bets << ",\n";
  out << "  \"draws_total\": " << c.draws << ",\n";
  out << "  \"per_lottery\": [";
  for (int l = 0; l < kLotteryCount; ++l)
    out << c.per_lottery[l] << (l + 1 < kLotteryCount ? ", " : "");
  out << "],\n";
  out << "  \"per_type\": ["
      << c.per_type[0] << ", " << c.per_type[1] << ", " << c.per_type[2]
      << "],\n";
  out << "  \"per_animalito\": [";
  for (int a = 0; a < kAnimalitoCount; ++a)
    out << c.per_animalito[a] << (a + 1 < kAnimalitoCount ? ", " : "");
  out << "],\n";
  out << "  \"per_terminal\": [";
  for (int t = 0; t < kTerminalCount; ++t)
    out << c.per_terminal[t] << (t + 1 < kTerminalCount ? ", " : "");
  out << "],\n";
  auto db_line = [&](const char* name, const MDB_stat& s) {
    out << "    {\"name\": \"" << name << "\", \"entries\": " << s.ms_entries
        << ", \"leaf_pages\": " << s.ms_leaf_pages
        << ", \"branch_pages\": " << s.ms_branch_pages
        << ", \"overflow_pages\": " << s.ms_overflow_pages
        << ", \"depth\": " << s.ms_depth << ", \"psize\": " << s.ms_psize
        << "}";
  };
  out << "  \"db_stats\": [\n";
  db_line("bets", s_bets); out << ",\n";
  db_line("i_animalito", s_ani); out << ",\n";
  db_line("i_terminal", s_ter); out << ",\n";
  db_line("i_taquilla", s_taq); out << ",\n";
  db_line("draws", s_drw); out << "\n  ],\n";
  out << "  \"file_bytes\": " << file_bytes << ",\n";
  out << "  \"duration_ms\": "
      << static_cast<uint64_t>(seconds * 1000.0) << ",\n";
  out << "  \"validation\": \"" << (pass ? "PASS" : "FAIL") << "\"\n";
  out << "}\n";
}

}  // namespace

/// @brief Punto de entrada del generador de datasets.
///
/// Flujo: interpreta argumentos → abre el entorno → genera sorteos y jugadas
/// por lotería → cierra → auto-prueba el orden de claves → escribe el
/// manifiesto → valida los totales.
///
/// @param[in] argc Cantidad de argumentos.
/// @param[in] argv Argumentos de línea de comandos.
/// @return 0 si todo salió bien, 3 si la validación final falló,
///         1 ante cualquier otro error.
int main(int argc, char** argv) {
  try {
    Params p = parse_args(argc, argv);
    if (p.taquillas == 0) throw std::runtime_error("--taquillas debe ser > 0");
    if (fs::exists(p.out))
      throw std::runtime_error(
          "el archivo de salida ya existe: " + p.out +
          " (bórralo o elige otro --out; me niego a mezclar datasets)");

    auto t0 = std::chrono::steady_clock::now();
    const fs::path out_dir = fs::path(p.out).parent_path();
    if (!out_dir.empty()) fs::create_directories(out_dir);

    std::cout << "Generando dataset: jugadas=" << p.bets << " años=" << p.years
              << " taquillas=" << p.taquillas << " semilla=" << p.seed << "\n";

    DbHandles db;
    open_env(p, db);

    Counters c;
    SplitMix64 rng(p.seed);
    WeightedPicker animals(animalito_weights());

    // Reparto exacto de las N jugadas entre las loterías.
    const uint64_t base = p.bets / kLotteryCount;
    const uint64_t rem = p.bets % kLotteryCount;

    const uint32_t days = p.years * 365;  // años planos de 365 días (documentado)

    // Peso total de la grilla día-hora (idéntico para todas las loterías).
    double w_total = 0.0;
    for (uint32_t d = 0; d < days; ++d) {
      int dow = (static_cast<int>(d) + kDowOffset) % 7;
      double wf = (dow == 5 || dow == 6) ? kWeekendFactor : 1.0;
      for (int h = 0; h < 24; ++h) w_total += kHourWeights[h] * wf;
    }

    MDB_txn* txn = nullptr;
    LMDB_CHECK(mdb_txn_begin(db.env, nullptr, 0, &txn));

    for (int l = 0; l < kLotteryCount; ++l) {
      const uint8_t lottery = static_cast<uint8_t>(l);
      const uint64_t n_l = base + (static_cast<uint64_t>(l) < rem ? 1 : 0);
      const LotteryConfig& slots = kLotterySlots[l];

      // ---- Sorteos de esta lotería ----------------------------------------
      for (uint32_t d = 0; d < days; ++d) {
        for (uint8_t s = 0; s < slots.count; ++s) {
          const uint8_t winner =
              static_cast<uint8_t>(animals.pick(rng.next_unit()));
          auto dk = keys::encode_draws_key(lottery, d, s);
          auto dv = keys::encode_draws_value(winner, winner);
          put(txn, db.draws, dk.data(), dk.size(), dv.data(), dv.size(), false);
          c.draws++;
        }
      }

      // ---- Jugadas de esta lotería, en orden cronológico ------------------
      uint64_t written_l = 0;
      double cum = 0.0;
      for (uint32_t d = 0; d < days && written_l < n_l; ++d) {
        const int dow = (static_cast<int>(d) + kDowOffset) % 7;
        const bool weekend = (dow == 5 || dow == 6);
        for (int h = 0; h < 24 && written_l < n_l; ++h) {
          const double w = kHourWeights[h] * (weekend ? kWeekendFactor : 1.0);
          cum += w;
          // Truco de conteo exacto: las diferencias entre metas acumuladas
          // nunca se desvían, así la lotería escribe exactamente n_l jugadas.
          const uint64_t upto = static_cast<uint64_t>(
              static_cast<double>(n_l) * (cum / w_total) + 0.5);
          const uint64_t cnt = (upto > written_l) ? (upto - written_l) : 0;
          if (cnt == 0) continue;

          // Amortigua las jugadas de la hora y ordénalas por marca de tiempo:
          // es obligatorio para que el flujo de claves de `bets` se mantenga
          // estrictamente ascendente y MDB_APPEND funcione.
          struct Pending { uint64_t ts; uint16_t taquilla; };
          std::vector<Pending> pend;
          pend.reserve(cnt);
          for (uint64_t j = 0; j < cnt; ++j) {
            const uint64_t ts =
                kTsEpoch + static_cast<uint64_t>(d) * 86400ULL +
                static_cast<uint64_t>(h) * 3600ULL + (rng.next() % 3600);
            pend.push_back(
                {ts, static_cast<uint16_t>(rng.next() % p.taquillas)});
          }
          std::sort(pend.begin(), pend.end(),
                    [](const Pending& a, const Pending& b) {
                      return a.ts == b.ts ? a.taquilla < b.taquilla
                                          : a.ts < b.ts;
                    });

          // seq desambigua dos jugadas del mismo segundo y la misma taquilla;
          // el mapa se puede limpiar cada hora porque las marcas de tiempo
          // son únicas dentro de cada bloque de una hora.
          std::unordered_map<uint64_t, uint16_t> seq_map;
          for (const Pending& q : pend) {
            const uint64_t sk = q.ts * 65536ULL + q.taquilla;
            const uint16_t seq = seq_map[sk]++;
            Bet b = make_bet(rng, animals, lottery, q.ts, q.taquilla, seq);
            write_bet(db, txn, b, c);
            ++written_l;
            if (written_l % p.batch_txn == 0 && written_l < n_l) {
              LMDB_CHECK(mdb_txn_commit(txn));
              LMDB_CHECK(mdb_txn_begin(db.env, nullptr, 0, &txn));
            }
          }
        }
      }
    }
    LMDB_CHECK(mdb_txn_commit(txn));

    const uint64_t file_bytes = static_cast<uint64_t>(fs::file_size(p.out));
    auto t1 = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(t1 - t0).count();

    self_test_key_order(db);

    // El manifiesto vive junto al dataset y se nombra a partir de él
    // (<nombre>.manifest.json), de modo que dos datasets en la misma carpeta
    // jamás se pisan la verdad absoluta el uno al otro.
    const std::string stem = fs::path(p.out).stem().string();
    const std::string manifest = out_dir.empty()
                                     ? stem + ".manifest.json"
                                     : (out_dir / (stem + ".manifest.json")).string();
    write_manifest(p, c, db, file_bytes, secs, manifest);

    std::cout << "jugadas_escritas=" << c.bets << " sorteos=" << c.draws << "\n";
    std::cout << "bytes_en_archivo=" << file_bytes << " ("
              << static_cast<double>(file_bytes) / (1024.0 * 1024.0)
              << " MB)\n";
    std::cout << "duracion_ms=" << static_cast<uint64_t>(secs * 1000.0)
              << "\n";
    std::cout << "manifiesto=" << manifest << "\n";

    mdb_dbi_close(db.env, db.bets);
    mdb_dbi_close(db.env, db.i_animalito);
    mdb_dbi_close(db.env, db.i_terminal);
    mdb_dbi_close(db.env, db.i_taquilla);
    mdb_dbi_close(db.env, db.draws);
    mdb_env_close(db.env);

    bool pass = (c.bets == p.bets);
    uint64_t lot_sum = 0;
    for (int l = 0; l < kLotteryCount; ++l) lot_sum += c.per_lottery[l];
    pass = pass && (lot_sum == p.bets);
    std::cout << "VALIDACIÓN: " << (pass ? "APROBADA" : "FALLÓ") << "\n";
    return pass ? 0 : 3;
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
}
