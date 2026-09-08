/// @file types.h
/// @brief Tipos del dominio y constantes para el dataset de jugadas de lotería.
///
/// El dataset modela un negocio de apuestas de lotería venezolana:
///   - Varias loterías realizan varios sorteos fijos al día ("horarios").
///   - Las taquillas (puntos de venta) reciben jugadas de tres tipos:
///       ANIMALITO (jugar a una de 37 figuras),
///       TERMINAL  (jugar a una terminación de 2 dígitos 00..99),
///       TRIPLETA  (jugar a tres figuras, sin importar el orden).
///   - Cada jugada se guarda una vez en la base maestra y una vez por cada
///     índice secundario.
#pragma once

#include <cstdint>

namespace lotto {

/// @brief Nombre de la sub-base maestra de jugadas.
inline constexpr const char* kDbBets = "bets";
/// @brief Nombre de la sub-base del índice por animalito.
inline constexpr const char* kDbAnimalitoIndex = "i_animalito";
/// @brief Nombre de la sub-base del índice por terminal.
inline constexpr const char* kDbTerminalIndex = "i_terminal";
/// @brief Nombre de la sub-base del índice por taquilla.
inline constexpr const char* kDbTaquillaIndex = "i_taquilla";
/// @brief Nombre de la sub-base de sorteos (los resultados).
inline constexpr const char* kDbDraws = "draws";

/// @brief Tope de sub-bases con nombre que se pasa a mdb_env_set_maxdbs().
inline constexpr unsigned kMaxSubDbs = 8;

/// @brief Cantidad de animalitos (ids 0..36).
inline constexpr int kAnimalitoCount = 37;
/// @brief Cantidad de terminales jugables (ids 0..99).
inline constexpr int kTerminalCount = 100;

/// @brief Las marcas de tiempo son segundos desde el 2020-01-01T00:00:00Z
///        (época del dataset).
inline constexpr uint64_t kTsEpoch = 1577836800ULL;
/// @brief El día 0 es el 2020-01-01, un miércoles: dow(día) = (día + 3) % 7
///        con 0 = domingo ... 6 = sábado.
inline constexpr int kDowOffset = 3;

/// @brief Loterías incluidas en el dataset sintético.
enum class Lottery : uint8_t {
  LottoActivo = 0,
  LaGranjita = 1,
  GuacharoActivo = 2,
  SelvaPlus = 3,
  LoteriaDeCaracas = 4,
  LottoRey = 5,
};

/// @brief Cantidad de loterías para las que el generador produce datos.
inline constexpr int kLotteryCount = 6;

/// @brief Tipo de jugada a la que se refiere una apuesta.
enum class BetType : uint8_t {
  Animalito = 0,  ///< Jugar a una sola figura.
  Terminal = 1,   ///< Jugar a una terminación de 2 dígitos 00..99.
  Tripleta = 2,   ///< Jugar a tres figuras (no importa el orden).
};

/// @brief Tamaño en bytes del valor de `bets` en disco.
inline constexpr int kBetsValueSize = 16;
/// @brief Tamaño en bytes de un valor de índice secundario en disco.
inline constexpr int kIndexValueSize = 8;
/// @brief Tamaño en bytes del valor de `draws` en disco.
inline constexpr int kDrawsValueSize = 2;

/// @brief Representación en memoria de una jugada (no es el formato en disco).
struct Bet {
  uint8_t lottery;      ///< Id de la lotería 0..5.
  uint64_t ts;          ///< Segundos desde kTsEpoch.
  uint16_t taquilla;    ///< Id de la taquilla (punto de venta).
  uint16_t seq;         ///< Desambiguador dentro de (ts, taquilla).
  BetType type;         ///< Tipo de jugada.
  uint8_t sel0, sel1, sel2;  ///< Selecciones; el significado depende del tipo.
  uint64_t amount_cents;     ///< Monto jugado en céntimos.
};

/// @brief Resultado de un sorteo (qué animalito salió).
struct Draw {
  uint8_t lottery;      ///< Id de la lotería.
  uint32_t date_days;   ///< Días desde kTsEpoch.
  uint8_t slot;         ///< Posición del sorteo dentro del día.
  uint8_t animalito;    ///< Animalito ganador 0..36.
  uint8_t number;       ///< Número ganador 0..36.
};

}  // namespace lotto
