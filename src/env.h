/// @file env.h
/// @brief Utilidades compartidas para abrir entornos LMDB del proyecto.
#pragma once

#include <lmdb.h>

#include <cstdint>
#include <stdexcept>
#include <string>

#include "types.h"

namespace lotto {

/// @brief Evalúa @p call y aborta lanzando una excepción clara si falla.
#define LMDB_CHECK(call)                                            \
  do {                                                              \
    int rc_ = (call);                                               \
    if (rc_ != MDB_SUCCESS) {                                       \
      throw std::runtime_error(std::string(#call) + " falló: " +    \
                               mdb_strerror(rc_));                  \
    }                                                               \
  } while (0)

/// @brief Entorno LMDB y manejadores de las sub-bases del proyecto.
struct DbHandles {
  MDB_env* env = nullptr;  ///< Entorno abierto.
  MDB_dbi bets{};          ///< Base maestra de jugadas.
  MDB_dbi i_animalito{};   ///< Índice por animalito.
  MDB_dbi i_terminal{};    ///< Índice por terminal.
  MDB_dbi i_taquilla{};    ///< Índice por taquilla.
  MDB_dbi draws{};         ///< Base de sorteos.
};

/// @brief Crea el entorno en modo solo lectura.
///
/// Nota de esta build de LMDB: los manejadores de sub-base abiertos en una
/// transacción de lectura dejan de ser válidos cuando esa transacción se
/// aborta (verificado empíricamente: mdb_stat devuelve EINVAL). Por eso cada
/// comando llama a abrir_subbases() dentro de la MISMA transacción que usa.
///
/// @param[in]  path Ruta del archivo LMDB (archivo único, MDB_NOSUBDIR).
/// @param[out] db   Estructura con el entorno listo; las sub-bases se
///                  abren por transacción.
/// @throws std::runtime_error si el archivo no existe o no es un entorno
///         LMDB válido.
inline void open_readonly(const std::string& path, DbHandles& db) {
  LMDB_CHECK(mdb_env_create(&db.env));
  LMDB_CHECK(mdb_env_set_maxdbs(db.env, kMaxSubDbs));
  LMDB_CHECK(
      mdb_env_open(db.env, path.c_str(), MDB_RDONLY | MDB_NOSUBDIR, 0664));
}

/// @brief Abre las cinco sub-bases dentro de la transacción dada.
///
/// Los manejadores resultantes solo son válidos mientras esa transacción
/// siga viva (comportamiento verificado de esta build de LMDB).
///
/// @param[in]    txn Transacción de lectura ya abierta.
/// @param[in,out] db  Estructura cuyos manejadores se llenan.
/// @throws std::runtime_error si alguna sub-base no existe.
inline void abrir_subbases(MDB_txn* txn, DbHandles& db) {
  LMDB_CHECK(mdb_dbi_open(txn, kDbBets, 0, &db.bets));
  LMDB_CHECK(mdb_dbi_open(txn, kDbAnimalitoIndex, 0, &db.i_animalito));
  LMDB_CHECK(mdb_dbi_open(txn, kDbTerminalIndex, 0, &db.i_terminal));
  LMDB_CHECK(mdb_dbi_open(txn, kDbTaquillaIndex, 0, &db.i_taquilla));
  LMDB_CHECK(mdb_dbi_open(txn, kDbDraws, 0, &db.draws));
}

/// @brief Cierra el entorno abierto con open_readonly().
/// @param[in,out] db Estructura a cerrar.
inline void close_env(DbHandles& db) {
  mdb_env_close(db.env);
}

/// @brief PRNG determinista splitmix64 (compartido por las herramientas).
class SplitMix64 {
 public:
  /// @brief Construye el generador a partir de una semilla.
  explicit SplitMix64(uint64_t seed) : state_(seed) {}
  /// @brief Produce el siguiente entero crudo de 64 bits.
  uint64_t next() {
    state_ += 0x9E3779B97F15ULL;
    uint64_t z = state_;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
  }

 private:
  uint64_t state_;
};

}  // namespace lotto
