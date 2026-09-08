/// @file query.cpp
/// @brief Capa de consultas sobre el dataset (Fase 3 del proyecto).
///
/// Comandos disponibles (todos abren el dataset en solo lectura):
///   info                       — estadísticas del entorno y de cada sub-base.
///   lookup   --lottery --ts --taquilla [--seq]
///                              — búsqueda puntual por clave exacta en bets.
///   range    --lottery --from --to
///                              — barrido por rango de fechas en bets.
///   animalito-stats --lottery [--animal]
///                              — frecuencia y monto por animalito (índice).
///   terminal-stats  --lottery  — frecuencia y monto por terminal (índice).
///   racha    --lottery --animal
///                              — cuánto lleva el animalito sin salir (draws).
///   taquilla --taquilla        — replay de un punto de venta (índice).
///   validate                   — valida TODOS los agregados contra el
///                                manifiesto del dataset (verdad absoluta).
///
/// La validación es la prueba de corrección de la fase: cada consulta de
/// agregados se compara contra los contadores exactos que guardó el
/// generador en <dataset>.manifest.json.

#include <lmdb.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "env.h"
#include "keys.h"
#include "types.h"

namespace fs = std::filesystem;
using namespace lotto;

namespace {

/// @brief Lee un entero de un campo escalar del manifiesto JSON.
/// @param[in] json Contenido completo del manifiesto.
/// @param[in] key  Nombre del campo (p. ej. "bets_total").
/// @return Valor del campo.
/// @throws std::runtime_error si el campo no existe o no es numérico.
uint64_t extract_u64(const std::string& json, const std::string& key) {
  const std::string needle = "\"" + key + "\":";
  size_t pos = json.find(needle);
  if (pos == std::string::npos)
    throw std::runtime_error("campo no encontrado en el manifiesto: " + key);
  pos += needle.size();
  while (pos < json.size() &&
         (json[pos] == ' ' || json[pos] == '\n' || json[pos] == '\t'))
    ++pos;
  size_t end = pos;
  while (end < json.size() && std::isdigit(static_cast<unsigned char>(json[end])))
    ++end;
  if (end == pos)
    throw std::runtime_error("valor no numérico para el campo: " + key);
  return std::stoull(json.substr(pos, end - pos));
}

/// @brief Lee un arreglo de enteros del manifiesto JSON.
/// @param[in] json Contenido completo del manifiesto.
/// @param[in] key  Nombre del campo (p. ej. "per_animalito").
/// @return Elementos del arreglo, en orden.
/// @throws std::runtime_error si el campo no existe o no es un arreglo.
std::vector<uint64_t> extract_array(const std::string& json,
                                    const std::string& key) {
  const std::string needle = "\"" + key + "\":";
  size_t pos = json.find(needle);
  if (pos == std::string::npos)
    throw std::runtime_error("campo no encontrado en el manifiesto: " + key);
  pos += needle.size();
  while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos])))
    ++pos;
  if (pos >= json.size() || json[pos] != '[')
    throw std::runtime_error("el campo no es un arreglo: " + key);
  ++pos;
  std::vector<uint64_t> out;
  while (pos < json.size()) {
    while (pos < json.size() &&
           (std::isspace(static_cast<unsigned char>(json[pos])) || json[pos] == ','))
      ++pos;
    if (json[pos] == ']') break;
    size_t end = pos;
    while (end < json.size() &&
           std::isdigit(static_cast<unsigned char>(json[end])))
      ++end;
    if (end == pos)
      throw std::runtime_error("elemento no numérico en: " + key);
    out.push_back(std::stoull(json.substr(pos, end - pos)));
    pos = end;
  }
  return out;
}

/// @brief Cursor con posicionamiento por rango y barrido por prefijo.
///
/// Encapsula el patrón repetido de las consultas: posicionarse en la primera
/// clave mayor o igual a un prefijo y caminar mientras el prefijo se
/// mantenga.
class Scannner {
 public:
  /// @brief Abre un cursor de solo lectura sobre la sub-base dada.
  Scannner(MDB_txn* txn, MDB_dbi dbi) {
    if (mdb_cursor_open(txn, dbi, &cur_) != MDB_SUCCESS) cur_ = nullptr;
  }
  ~Scannner() {
    if (cur_) mdb_cursor_close(cur_);
  }
  /// @brief Se posiciona en la primera clave >= @p key.
  /// @return true si quedó en una clave válida.
  bool seek(const void* key, size_t len) {
    if (!cur_) return false;
    MDB_val k{len, const_cast<void*>(key)};
    MDB_val v{};
    return mdb_cursor_get(cur_, &k, &v, MDB_SET_RANGE) == MDB_SUCCESS;
  }
  /// @brief Lee la clave/valor actuales.
  bool read(MDB_val& k, MDB_val& v) {
    if (!cur_) return false;
    return mdb_cursor_get(cur_, &k, &v, MDB_GET_CURRENT) == MDB_SUCCESS;
  }
  /// @brief Avanza a la siguiente entrada.
  /// @return true si hay siguiente.
  bool next(MDB_val& k, MDB_val& v) {
    if (!cur_) return false;
    return mdb_cursor_get(cur_, &k, &v, MDB_NEXT) == MDB_SUCCESS;
  }

 private:
  MDB_cursor* cur_ = nullptr;
};

// ---- comandos --------------------------------------------------------------

/// @brief `info`: estadísticas del entorno y de cada sub-base + tamaño.
void cmd_info(DbHandles& db, const std::string& path) {
  MDB_txn* txn = nullptr;
  LMDB_CHECK(mdb_txn_begin(db.env, nullptr, MDB_RDONLY, &txn));
  abrir_subbases(txn, db);

  MDB_envinfo info{};
  LMDB_CHECK(mdb_env_info(db.env, &info));
  std::cout << "map_size_gib=" << (info.me_mapsize / (1024.0 * 1024 * 1024))
            << " ultima_pagina=" << info.me_last_pgno << "\n";

  auto dump = [&](const char* name, MDB_dbi dbi) {
    MDB_stat st{};
    LMDB_CHECK(mdb_stat(txn, dbi, &st));
    std::cout << name << ": entradas=" << st.ms_entries
              << " hojas=" << st.ms_leaf_pages
              << " ramas=" << st.ms_branch_pages
              << " overflow=" << st.ms_overflow_pages
              << " profundidad=" << st.ms_depth << "\n";
  };
  dump("bets", db.bets);
  dump("i_animalito", db.i_animalito);
  dump("i_terminal", db.i_terminal);
  dump("i_taquilla", db.i_taquilla);
  dump("draws", db.draws);
  mdb_txn_abort(txn);

  std::cout << "bytes_en_archivo=" << fs::file_size(path) << "\n";
}

/// @brief `lookup`: búsqueda puntual por clave exacta en `bets`.
void cmd_lookup(DbHandles& db, uint8_t lottery, uint64_t ts,
                uint16_t taquilla, uint16_t seq) {
  auto key = keys::encode_bets_key(lottery, ts, taquilla, seq);
  MDB_txn* txn = nullptr;
  LMDB_CHECK(mdb_txn_begin(db.env, nullptr, MDB_RDONLY, &txn));
  abrir_subbases(txn, db);
  MDB_val k{key.size(), key.data()};
  MDB_val v{};
  int rc = mdb_get(txn, db.bets, &k, &v);
  if (rc == MDB_NOTFOUND) {
    std::cout << "NO ENCONTRADA: lotería=" << (int)lottery << " ts=" << ts
              << " taquilla=" << taquilla << " seq=" << seq << "\n";
  } else {
    LMDB_CHECK(rc);
    uint8_t type, s0, s1, s2;
    uint64_t amount;
    keys::decode_bets_value(static_cast<const uint8_t*>(v.mv_data), v.mv_size,
                            type, s0, s1, s2, amount);
    std::cout << "ENCONTRADA: tipo=" << (int)type << " selecciones=("
              << (int)s0 << "," << (int)s1 << "," << (int)s2 << ")"
              << " monto_cents=" << amount << "\n";
  }
  mdb_txn_abort(txn);
}

/// @brief `range`: barrido por rango de fechas dentro de una lotería.
void cmd_range(DbHandles& db, uint8_t lottery, uint64_t from, uint64_t to) {
  auto lo = keys::encode_bets_key(lottery, from, 0, 0);
  MDB_txn* txn = nullptr;
  LMDB_CHECK(mdb_txn_begin(db.env, nullptr, MDB_RDONLY, &txn));
  abrir_subbases(txn, db);
  Scannner s(txn, db.bets);
  if (!s.seek(lo.data(), lo.size())) {
    std::cout << "rango vacío\n";
    mdb_txn_abort(txn);
    return;
  }
  uint64_t count = 0, total = 0;
  MDB_val k{}, v{};
  while (s.read(k, v)) {
    keys::BetsKey bk = keys::decode_bets_key(
        static_cast<const uint8_t*>(k.mv_data), k.mv_size);
    if (bk.lottery != lottery || bk.ts > to) break;
    ++count;
    total += keys::get_le64(static_cast<const uint8_t*>(v.mv_data) + 4);
    if (!s.next(k, v)) break;
  }
  std::cout << "jugadas=" << count << " monto_total_cents=" << total
            << " rango=[lotería " << (int)lottery << ", ts " << from << ".."
            << to << "]\n";
  mdb_txn_abort(txn);
}

/// @brief `animalito-stats`: frecuencia y monto por animalito usando el índice.
void cmd_animalito_stats(DbHandles& db, uint8_t lottery, int animal) {
  MDB_txn* txn = nullptr;
  LMDB_CHECK(mdb_txn_begin(db.env, nullptr, MDB_RDONLY, &txn));
  abrir_subbases(txn, db);
  Scannner s(txn, db.i_animalito);
  uint64_t global_count = 0, global_total = 0;
  int desde = (animal >= 0) ? animal : 0;
  int hasta = (animal >= 0) ? animal : kAnimalitoCount - 1;
  for (int a = desde; a <= hasta; ++a) {
    auto lo = keys::encode_index_key(lottery, static_cast<uint8_t>(a), 0, 0, 0);
    if (!s.seek(lo.data(), lo.size())) continue;
    uint64_t count = 0, total = 0;
    MDB_val k{}, v{};
    while (s.read(k, v)) {
      keys::IndexKey ik = keys::decode_index_key(
          static_cast<const uint8_t*>(k.mv_data), k.mv_size);
      if (ik.domain != lottery || ik.term != static_cast<uint8_t>(a)) break;
      ++count;
      total += keys::get_le64(static_cast<const uint8_t*>(v.mv_data));
      if (!s.next(k, v)) break;
    }
    if (animal < 0 && count > 0)
      std::cout << "animalito " << a << ": jugadas=" << count
                << " monto_cents=" << total << "\n";
    if (animal >= 0)
      std::cout << "animalito " << a << ": jugadas=" << count
                << " monto_cents=" << total << "\n";
    global_count += count;
    global_total += total;
  }
  if (animal < 0)
    std::cout << "total_loteria " << (int)lottery << ": jugadas="
              << global_count << " monto_cents=" << global_total << "\n";
  mdb_txn_abort(txn);
}

/// @brief `terminal-stats`: frecuencia y monto por terminal usando el índice.
void cmd_terminal_stats(DbHandles& db, uint8_t lottery) {
  MDB_txn* txn = nullptr;
  LMDB_CHECK(mdb_txn_begin(db.env, nullptr, MDB_RDONLY, &txn));
  abrir_subbases(txn, db);
  Scannner s(txn, db.i_terminal);
  uint64_t global_count = 0, global_total = 0;
  for (int t = 0; t < kTerminalCount; ++t) {
    auto lo = keys::encode_index_key(lottery, static_cast<uint8_t>(t), 0, 0, 0);
    if (!s.seek(lo.data(), lo.size())) continue;
    uint64_t count = 0, total = 0;
    MDB_val k{}, v{};
    while (s.read(k, v)) {
      keys::IndexKey ik = keys::decode_index_key(
          static_cast<const uint8_t*>(k.mv_data), k.mv_size);
      if (ik.domain != lottery || ik.term != static_cast<uint8_t>(t)) break;
      ++count;
      total += keys::get_le64(static_cast<const uint8_t*>(v.mv_data));
      if (!s.next(k, v)) break;
    }
    if (count > 0)
      std::cout << "terminal " << t << ": jugadas=" << count
                << " monto_cents=" << total << "\n";
    global_count += count;
    global_total += total;
  }
  std::cout << "total_loteria " << (int)lottery << ": jugadas=" << global_count
            << " monto_cents=" << global_total << "\n";
  mdb_txn_abort(txn);
}

/// @brief `racha`: cuántos días lleva el animalito sin salir en los sorteos.
void cmd_racha(DbHandles& db, uint8_t lottery, uint8_t animal) {
  MDB_txn* txn = nullptr;
  LMDB_CHECK(mdb_txn_begin(db.env, nullptr, MDB_RDONLY, &txn));
  abrir_subbases(txn, db);
  Scannner s(txn, db.draws);
  auto lo = keys::encode_draws_key(lottery, 0, 0);
  if (!s.seek(lo.data(), lo.size())) {
    std::cout << "sin sorteos para la lotería " << (int)lottery << "\n";
    mdb_txn_abort(txn);
    return;
  }
  uint64_t apariciones = 0;
  uint32_t ultimo_dia = 0, dia_salida = 0;
  MDB_val k{}, v{};
  while (s.read(k, v)) {
    keys::DrawsKey dk = keys::decode_draws_key(
        static_cast<const uint8_t*>(k.mv_data), k.mv_size);
    if (dk.lottery != lottery) break;
    ultimo_dia = std::max(ultimo_dia, dk.date_days);
    const uint8_t* dv = static_cast<const uint8_t*>(v.mv_data);
    if (v.mv_size == 2 && dv[0] == animal) {
      ++apariciones;
      dia_salida = std::max(dia_salida, dk.date_days);
    }
    if (!s.next(k, v)) break;
  }
  std::cout << "animalito " << (int)animal << " en lotería " << (int)lottery
            << ": salió " << apariciones << " veces; última salida el día "
            << dia_salida << " (el dataset llega al día " << ultimo_dia
            << "); días sin salir=" << (ultimo_dia - dia_salida) << "\n";
  mdb_txn_abort(txn);
}

/// @brief `taquilla`: replay completo de un punto de venta.
void cmd_taquilla(DbHandles& db, uint16_t taquilla) {
  MDB_txn* txn = nullptr;
  LMDB_CHECK(mdb_txn_begin(db.env, nullptr, MDB_RDONLY, &txn));
  abrir_subbases(txn, db);
  Scannner s(txn, db.i_taquilla);
  auto lo = keys::encode_taquilla_key(taquilla, 0, 0, 0);
  uint64_t count = 0, total = 0;
  if (s.seek(lo.data(), lo.size())) {
    MDB_val k{}, v{};
    while (s.read(k, v)) {
      keys::TaquillaKey tk = keys::decode_taquilla_key(
          static_cast<const uint8_t*>(k.mv_data), k.mv_size);
      if (tk.taquilla != taquilla) break;
      ++count;
      total += keys::get_le64(static_cast<const uint8_t*>(v.mv_data));
      if (!s.next(k, v)) break;
    }
  }
  std::cout << "taquilla " << taquilla << ": jugadas=" << count
            << " monto_cents=" << total << "\n";
  mdb_txn_abort(txn);
}

/// @brief `validate`: compara todos los agregados del dataset contra el
///        manifiesto (verdad absoluta guardada por el generador).
/// @param[in] db            Manejadores del dataset abierto.
/// @param[in] manifest_path Ruta del manifiesto JSON.
/// @return true si TODAS las comprobaciones pasan.
bool cmd_validate(DbHandles& db, const std::string& manifest_path) {
  std::ifstream in(manifest_path);
  if (!in)
    throw std::runtime_error("no se pudo abrir el manifiesto: " + manifest_path);
  std::string json((std::istreambuf_iterator<char>(in)),
                   std::istreambuf_iterator<char>());

  const uint64_t m_bets = extract_u64(json, "bets_total");
  const uint64_t m_draws = extract_u64(json, "draws_total");
  const auto m_per_lottery = extract_array(json, "per_lottery");
  const auto m_per_type = extract_array(json, "per_type");
  const auto m_per_animalito = extract_array(json, "per_animalito");
  const auto m_per_terminal = extract_array(json, "per_terminal");

  int ok = 0, checks = 0;
  auto check = [&](bool cond, const std::string& label, uint64_t got,
                   uint64_t want) {
    ++checks;
    if (cond) {
      ++ok;
      std::cout << "[OK] " << label << " (" << got << ")\n";
    } else {
      std::cout << "[FALLA] " << label << ": dataset=" << got
                << " manifiesto=" << want << "\n";
    }
  };

  MDB_txn* txn = nullptr;
  LMDB_CHECK(mdb_txn_begin(db.env, nullptr, MDB_RDONLY, &txn));
  abrir_subbases(txn, db);

  // Totales por sub-base.
  MDB_stat sb{}, st_{}, sq{}, sd{};
  LMDB_CHECK(mdb_stat(txn, db.bets, &sb));
  LMDB_CHECK(mdb_stat(txn, db.i_terminal, &st_));
  LMDB_CHECK(mdb_stat(txn, db.i_taquilla, &sq));
  LMDB_CHECK(mdb_stat(txn, db.draws, &sd));
  check(sb.ms_entries == m_bets, "bets.total == manifiesto.bets_total",
        sb.ms_entries, m_bets);
  check(sd.ms_entries == m_draws, "draws.total == manifiesto.draws_total",
        sd.ms_entries, m_draws);
  check(sq.ms_entries == m_bets, "i_taquilla.total == bets.total",
        sq.ms_entries, m_bets);

  // Un solo barrido de bets: lotería y tipo por jugada.
  uint64_t per_lot[kLotteryCount] = {};
  uint64_t per_tipo[3] = {};
  {
    Scannner s(txn, db.bets);
    MDB_val k{}, v{};
    if (s.next(k, v)) {  // MDB_FIRST: next() sin seek previo da la primera
      do {
        keys::BetsKey bk = keys::decode_bets_key(
            static_cast<const uint8_t*>(k.mv_data), k.mv_size);
        per_lot[bk.lottery]++;
        per_tipo[static_cast<const uint8_t*>(v.mv_data)[0]]++;
      } while (s.next(k, v));
    }
  }
  for (int l = 0; l < kLotteryCount; ++l) {
    uint64_t want = l < (int)m_per_lottery.size() ? m_per_lottery[l] : 0;
    check(per_lot[l] == want,
          "per_lottery[" + std::to_string(l) + "] coincide", per_lot[l], want);
  }
  for (int t = 0; t < 3; ++t) {
    uint64_t want = t < (int)m_per_type.size() ? m_per_type[t] : 0;
    check(per_tipo[t] == want, "per_type[" + std::to_string(t) + "] coincide",
          per_tipo[t], want);
  }

  // Agregados por animalito (índice completo).
  {
    uint64_t per_ani[kAnimalitoCount] = {};
    Scannner s(txn, db.i_animalito);
    MDB_val k{}, v{};
    if (s.next(k, v)) {
      do {
        keys::IndexKey ik = keys::decode_index_key(
            static_cast<const uint8_t*>(k.mv_data), k.mv_size);
        per_ani[ik.term]++;
      } while (s.next(k, v));
    }
    for (int a = 0; a < kAnimalitoCount; ++a) {
      uint64_t want =
          a < (int)m_per_animalito.size() ? m_per_animalito[a] : 0;
      check(per_ani[a] == want,
            "per_animalito[" + std::to_string(a) + "] coincide", per_ani[a],
            want);
    }
  }

  // Agregados por terminal (índice completo).
  {
    uint64_t per_ter[kTerminalCount] = {};
    Scannner s(txn, db.i_terminal);
    MDB_val k{}, v{};
    if (s.next(k, v)) {
      do {
        keys::IndexKey ik = keys::decode_index_key(
            static_cast<const uint8_t*>(k.mv_data), k.mv_size);
        per_ter[ik.term]++;
      } while (s.next(k, v));
    }
    for (int t = 0; t < kTerminalCount; ++t) {
      uint64_t want = t < (int)m_per_terminal.size() ? m_per_terminal[t] : 0;
      check(per_ter[t] == want,
            "per_terminal[" + std::to_string(t) + "] coincide", per_ter[t],
            want);
    }
  }

  mdb_txn_abort(txn);
  std::cout << "VALIDACIÓN: " << (ok == checks ? "APROBADA" : "FALLÓ") << " ("
            << ok << "/" << checks << ")\n";
  return ok == checks;
}

}  // namespace

/// @brief Punto de entrada: interpreta argumentos y despacha el comando.
int main(int argc, char** argv) {
  try {
    std::string db_path, command;
    std::map<std::string, std::string> flags;
    for (int i = 1; i < argc; ++i) {
      std::string a = argv[i];
      auto val = [&]() -> std::string {
        if (i + 1 >= argc)
          throw std::runtime_error(a + " requiere un valor");
        return argv[++i];
      };
      if (a == "--db") db_path = val();
      else if (a.rfind("--", 0) == 0) flags[a] = val();
      else if (command.empty()) command = a;
      else throw std::runtime_error("argumento desconocido: " + a);
    }
    if (db_path.empty()) throw std::runtime_error("indica --db <dataset>");
    if (command.empty()) command = "info";

    DbHandles db;
    open_readonly(db_path, db);

    int exit_code = 0;
    if (command == "info") {
      cmd_info(db, db_path);
    } else if (command == "lookup") {
      cmd_lookup(db,
                 static_cast<uint8_t>(std::stoull(flags["--lottery"])),
                 std::stoull(flags["--ts"]),
                 static_cast<uint16_t>(std::stoull(flags["--taquilla"])),
                 static_cast<uint16_t>(
                     flags.count("--seq") ? std::stoull(flags["--seq"]) : 0));
    } else if (command == "range") {
      cmd_range(db, static_cast<uint8_t>(std::stoull(flags["--lottery"])),
                std::stoull(flags["--from"]), std::stoull(flags["--to"]));
    } else if (command == "animalito-stats") {
      int animal = flags.count("--animal") ? std::stoi(flags["--animal"]) : -1;
      cmd_animalito_stats(
          db, static_cast<uint8_t>(std::stoull(flags["--lottery"])), animal);
    } else if (command == "terminal-stats") {
      cmd_terminal_stats(
          db, static_cast<uint8_t>(std::stoull(flags["--lottery"])));
    } else if (command == "racha") {
      cmd_racha(db, static_cast<uint8_t>(std::stoull(flags["--lottery"])),
                static_cast<uint8_t>(std::stoull(flags["--animal"])));
    } else if (command == "taquilla") {
      cmd_taquilla(db,
                   static_cast<uint16_t>(std::stoull(flags["--taquilla"])));
    } else if (command == "validate") {
      std::string manifest = flags.count("--manifest")
                                 ? flags["--manifest"]
                                 : (fs::path(db_path).parent_path() /
                                    (fs::path(db_path).stem().string() +
                                     ".manifest.json"))
                                       .string();
      std::cout << "dataset=" << db_path << " manifiesto=" << manifest << "\n";
      exit_code = cmd_validate(db, manifest) ? 0 : 3;
    } else {
      throw std::runtime_error(
          "comando desconocido: " + command +
          " (info | lookup | range | animalito-stats | terminal-stats | "
          "racha | taquilla | validate)");
    }
    close_env(db);
    return exit_code;
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
}
