/// @file keys.h
/// @brief Codificación binaria de claves y valores para las bases LMDB.
///
/// REGLA CRÍTICA: LMDB compara las claves con memcmp (bytes crudos), no como
/// valores numéricos. Por eso todo entero multi-byte dentro de una CLAVE se
/// codifica en BIG-ENDIAN: así el orden lexicográfico de los bytes coincide
/// con el orden numérico, y los barridos por rango de fecha funcionan bien.
///
/// Los VALORES se codifican en LITTLE-ENDIAN de forma explícita, para que el
/// resultado sea idéntico en cualquier máquina. Todos los bytes de relleno se
/// ponen en cero, de modo que entradas iguales siempre producen claves
/// iguales.
///
/// Formato de las claves (tamaños en bytes):
///   bets        clave (13): lotería u8 | ts u64 | taquilla u16 | seq u16
///   i_animalito clave (16): lotería u8 | animalito u8 | ts u64 | taquilla
///                           u16 | seq u16 | relleno u16(0)
///   i_terminal  clave (16): lotería u8 | terminal u8 | ts u64 | taquilla
///                           u16 | seq u16 | relleno u16(0)
///   i_taquilla  clave (16): taquilla u16 | ts u64 | lotería u8 | seq u16 |
///                           relleno 3 bytes(0)
///   draws       clave (6):  lotería u8 | date_days u32 | slot u8
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

namespace lotto {
namespace keys {

// ---- Primitivas BIG-endian (para CLAVES) -----------------------------------

/// @brief Serializa un entero de 16 bits en big-endian dentro de @p p.
/// @param[out] p Búfer destino (al menos 2 bytes escribibles).
/// @param[in]  v Valor a codificar.
inline void put_be16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v >> 8);
  p[1] = static_cast<uint8_t>(v);
}

/// @brief Serializa un entero de 32 bits en big-endian dentro de @p p.
/// @param[out] p Búfer destino (al menos 4 bytes escribibles).
/// @param[in]  v Valor a codificar.
inline void put_be32(uint8_t* p, uint32_t v) {
  for (int i = 3; i >= 0; --i) {
    p[i] = static_cast<uint8_t>(v & 0xFF);
    v >>= 8;
  }
}

/// @brief Serializa un entero de 64 bits en big-endian dentro de @p p.
/// @param[out] p Búfer destino (al menos 8 bytes escribibles).
/// @param[in]  v Valor a codificar.
inline void put_be64(uint8_t* p, uint64_t v) {
  for (int i = 7; i >= 0; --i) {
    p[i] = static_cast<uint8_t>(v & 0xFF);
    v >>= 8;
  }
}

/// @brief Lee un entero de 16 bits desde bytes big-endian.
/// @param[in] p Búfer origen (al menos 2 bytes legibles).
/// @return Valor decodificado.
inline uint16_t get_be16(const uint8_t* p) {
  return static_cast<uint16_t>((uint16_t(p[0]) << 8) | uint16_t(p[1]));
}

/// @brief Lee un entero de 32 bits desde bytes big-endian.
/// @param[in] p Búfer origen (al menos 4 bytes legibles).
/// @return Valor decodificado.
inline uint32_t get_be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
         (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

/// @brief Lee un entero de 64 bits desde bytes big-endian.
/// @param[in] p Búfer origen (al menos 8 bytes legibles).
/// @return Valor decodificado.
inline uint64_t get_be64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v = (v << 8) | uint64_t(p[i]);
  return v;
}

// ---- Primitivas LITTLE-endian (para VALORES) -------------------------------

/// @brief Serializa un entero de 64 bits en little-endian dentro de @p p.
/// @param[out] p Búfer destino (al menos 8 bytes escribibles).
/// @param[in]  v Valor a codificar.
inline void put_le64(uint8_t* p, uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    p[i] = static_cast<uint8_t>(v & 0xFF);
    v >>= 8;
  }
}

/// @brief Lee un entero de 64 bits desde bytes little-endian.
/// @param[in] p Búfer origen (al menos 8 bytes legibles).
/// @return Valor decodificado.
inline uint64_t get_le64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | uint64_t(p[i]);
  return v;
}

// ---- Clave de `bets` (13 bytes) --------------------------------------------

/// @brief Tamaño en bytes de una clave de `bets`.
inline constexpr size_t kBetsKeySize = 13;

/// @brief Forma decodificada de una clave de `bets`.
struct BetsKey {
  uint8_t lottery;    ///< Id de la lotería.
  uint64_t ts;        ///< Segundos desde la época del dataset.
  uint16_t taquilla;  ///< Id de la taquilla.
  uint16_t seq;       ///< Desambiguador dentro de (ts, taquilla).
};

/// @brief Codifica la clave maestra de una jugada (asciende con el tiempo,
///        lo que habilita MDB_APPEND).
/// @param[in] lottery  Id de la lotería 0..5.
/// @param[in] ts       Segundos desde la época del dataset.
/// @param[in] taquilla Id de la taquilla.
/// @param[in] seq      Desambiguador dentro de (ts, taquilla).
/// @return Clave big-endian de 13 bytes.
inline std::array<uint8_t, kBetsKeySize> encode_bets_key(uint8_t lottery,
                                                        uint64_t ts,
                                                        uint16_t taquilla,
                                                        uint16_t seq) {
  std::array<uint8_t, kBetsKeySize> k{};
  size_t o = 0;
  k[o++] = lottery;
  put_be64(&k[o], ts);
  o += 8;
  put_be16(&k[o], taquilla);
  o += 2;
  put_be16(&k[o], seq);
  return k;
}

/// @brief Decodifica una clave de `bets`.
/// @param[in] p   Bytes crudos de la clave.
/// @param[in] len Largo de la clave; debe ser igual a kBetsKeySize.
/// @return Campos decodificados.
/// @throws std::runtime_error si @p len no es kBetsKeySize.
inline BetsKey decode_bets_key(const uint8_t* p, size_t len) {
  if (len != kBetsKeySize) throw std::runtime_error("largo de clave bets inválido");
  return BetsKey{p[0], get_be64(p + 1), get_be16(p + 9), get_be16(p + 11)};
}

// ---- Claves de índices secundarios (16 bytes, misma forma) -----------------

/// @brief Tamaño en bytes de una clave de índice secundario.
inline constexpr size_t kIndexKeySize = 16;

/// @brief Forma decodificada de una clave de `i_animalito` / `i_terminal`.
struct IndexKey {
  uint8_t domain;    ///< Id de la lotería.
  uint8_t term;      ///< Id del animalito (i_animalito) o del terminal (i_terminal).
  uint64_t ts;       ///< Segundos desde la época del dataset.
  uint16_t taquilla; ///< Id de la taquilla.
  uint16_t seq;      ///< Desambiguador dentro de (ts, taquilla).
};

/// @brief Codifica una clave de índice para `i_animalito` o `i_terminal`.
///
/// La clave arranca por el campo por el que se indexa, de modo que todas las
/// jugadas de un mismo animalito (o terminal) quedan contiguas en el orden de
/// claves — justo lo que necesitan las consultas de frecuencia y de rachas.
///
/// @param[in] domain   Id de la lotería.
/// @param[in] term     Id del animalito o del terminal.
/// @param[in] ts       Segundos desde la época del dataset.
/// @param[in] taquilla Id de la taquilla.
/// @param[in] seq      Desambiguador dentro de (ts, taquilla).
/// @return Clave big-endian de 16 bytes con relleno en cero.
inline std::array<uint8_t, kIndexKeySize> encode_index_key(uint8_t domain,
                                                          uint8_t term,
                                                          uint64_t ts,
                                                          uint16_t taquilla,
                                                          uint16_t seq) {
  std::array<uint8_t, kIndexKeySize> k{};
  size_t o = 0;
  k[o++] = domain;
  k[o++] = term;
  put_be64(&k[o], ts);
  o += 8;
  put_be16(&k[o], taquilla);
  o += 2;
  put_be16(&k[o], seq);
  // k[14..15] quedan en cero (relleno) para claves deterministas.
  return k;
}

/// @brief Decodifica una clave de índice `i_animalito` / `i_terminal`.
/// @param[in] p   Bytes crudos de la clave.
/// @param[in] len Largo de la clave; debe ser igual a kIndexKeySize.
/// @return Campos decodificados.
/// @throws std::runtime_error si @p len no es kIndexKeySize.
inline IndexKey decode_index_key(const uint8_t* p, size_t len) {
  if (len != kIndexKeySize) throw std::runtime_error("largo de clave de índice inválido");
  return IndexKey{p[0], p[1], get_be64(p + 2), get_be16(p + 10),
                  get_be16(p + 12)};
}

// ---- Clave de `i_taquilla` (16 bytes) --------------------------------------

/// @brief Forma decodificada de una clave de `i_taquilla`.
struct TaquillaKey {
  uint16_t taquilla;  ///< Id de la taquilla.
  uint64_t ts;        ///< Segundos desde la época del dataset.
  uint8_t lottery;    ///< Id de la lotería.
  uint16_t seq;       ///< Desambiguador dentro de (ts, taquilla).
};

/// @brief Codifica una clave de `i_taquilla` (todas las jugadas de un mismo
///        punto de venta quedan contiguas).
/// @param[in] taquilla Id de la taquilla.
/// @param[in] ts       Segundos desde la época del dataset.
/// @param[in] lottery  Id de la lotería.
/// @param[in] seq      Desambiguador dentro de (ts, taquilla).
/// @return Clave big-endian de 16 bytes con relleno en cero.
inline std::array<uint8_t, kIndexKeySize> encode_taquilla_key(
    uint16_t taquilla, uint64_t ts, uint8_t lottery, uint16_t seq) {
  std::array<uint8_t, kIndexKeySize> k{};
  size_t o = 0;
  put_be16(&k[o], taquilla);
  o += 2;
  put_be64(&k[o], ts);
  o += 8;
  k[o++] = lottery;
  put_be16(&k[o], seq);
  // k[13..15] quedan en cero (relleno) para claves deterministas.
  return k;
}

/// @brief Decodifica una clave de `i_taquilla`.
/// @param[in] p   Bytes crudos de la clave.
/// @param[in] len Largo de la clave; debe ser igual a kIndexKeySize.
/// @return Campos decodificados.
/// @throws std::runtime_error si @p len no es kIndexKeySize.
inline TaquillaKey decode_taquilla_key(const uint8_t* p, size_t len) {
  if (len != kIndexKeySize)
    throw std::runtime_error("largo de clave taquilla inválido");
  return TaquillaKey{get_be16(p), get_be64(p + 2), p[10], get_be16(p + 11)};
}

// ---- Clave de `draws` (6 bytes) --------------------------------------------

/// @brief Tamaño en bytes de una clave de `draws`.
inline constexpr size_t kDrawsKeySize = 6;

/// @brief Forma decodificada de una clave de `draws`.
struct DrawsKey {
  uint8_t lottery;    ///< Id de la lotería.
  uint32_t date_days; ///< Días desde la época del dataset.
  uint8_t slot;       ///< Posición del sorteo dentro del día.
};

/// @brief Codifica una clave de `draws` (lotería + día + posición del sorteo).
/// @param[in] lottery   Id de la lotería.
/// @param[in] date_days Días desde la época del dataset.
/// @param[in] slot      Posición del sorteo dentro del día.
/// @return Clave big-endian de 6 bytes.
inline std::array<uint8_t, kDrawsKeySize> encode_draws_key(uint8_t lottery,
                                                          uint32_t date_days,
                                                          uint8_t slot) {
  std::array<uint8_t, kDrawsKeySize> k{};
  k[0] = lottery;
  put_be32(&k[1], date_days);
  k[5] = slot;
  return k;
}

/// @brief Decodifica una clave de `draws`.
/// @param[in] p   Bytes crudos de la clave.
/// @param[in] len Largo de la clave; debe ser igual a kDrawsKeySize.
/// @return Campos decodificados.
/// @throws std::runtime_error si @p len no es kDrawsKeySize.
inline DrawsKey decode_draws_key(const uint8_t* p, size_t len) {
  if (len != kDrawsKeySize) throw std::runtime_error("largo de clave draws inválido");
  return DrawsKey{p[0], get_be32(p + 1), p[5]};
}

// ---- Valores ---------------------------------------------------------------

/// @brief Codifica el valor de `bets`: tipo, selecciones, monto (LE) y bytes
///        reservados en cero.
/// @param[in] type         BetType como u8.
/// @param[in] sel0         Selección principal (animalito o terminal).
/// @param[in] sel1         Segundo animal de una tripleta (0 si no aplica).
/// @param[in] sel2         Tercer animal de una tripleta (0 si no aplica).
/// @param[in] amount_cents Monto jugado en céntimos.
/// @return Valor de 16 bytes.
inline std::array<uint8_t, 16> encode_bets_value(uint8_t type, uint8_t sel0,
                                                uint8_t sel1, uint8_t sel2,
                                                uint64_t amount_cents) {
  std::array<uint8_t, 16> v{};
  v[0] = type;
  v[1] = sel0;
  v[2] = sel1;
  v[3] = sel2;
  put_le64(&v[4], amount_cents);
  // v[12..15] quedan en cero (reservados).
  return v;
}

/// @brief Decodifica el valor de `bets`.
/// @param[in]  p            Bytes crudos del valor.
/// @param[in]  len          Largo del valor; debe ser 16.
/// @param[out] type         Tipo de jugada.
/// @param[out] sel0         Selección principal.
/// @param[out] sel1         Segundo animal de una tripleta.
/// @param[out] sel2         Tercer animal de una tripleta.
/// @param[out] amount_cents Monto jugado en céntimos.
/// @throws std::runtime_error si @p len no es 16.
inline void decode_bets_value(const uint8_t* p, size_t len, uint8_t& type,
                              uint8_t& sel0, uint8_t& sel1, uint8_t& sel2,
                              uint64_t& amount_cents) {
  if (len != 16) throw std::runtime_error("largo de valor bets inválido");
  type = p[0];
  sel0 = p[1];
  sel1 = p[2];
  sel2 = p[3];
  amount_cents = get_le64(p + 4);
}

/// @brief Codifica el valor de un índice: el monto jugado en céntimos (LE).
/// @param[in] amount_cents Monto jugado en céntimos.
/// @return Valor de 8 bytes.
inline std::array<uint8_t, 8> encode_index_value(uint64_t amount_cents) {
  std::array<uint8_t, 8> v{};
  put_le64(v.data(), amount_cents);
  return v;
}

/// @brief Codifica el valor de `draws` (bytes crudos: animalito, número).
/// @param[in] animalito Animalito ganador.
/// @param[in] number    Número ganador.
/// @return Valor de 2 bytes.
inline std::array<uint8_t, 2> encode_draws_value(uint8_t animalito,
                                                uint8_t number) {
  return {animalito, number};
}

}  // namespace keys
}  // namespace lotto
