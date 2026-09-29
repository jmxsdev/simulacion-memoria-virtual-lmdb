# Documentación del proyecto

Material de estudio y apoyo para la defensa oral del proyecto **Simulación de
memoria virtual con LMDB** (Universidad de Carabobo, Arquitectura del
Computador, Prof. José Canache, Proyecto N.° 13). El dominio es apuestas de
lotería venezolana: taquillas, jugadas, animalitos, sorteos, rachas, terminales
y tripletas.

Estos documentos explican, en orden, cómo se construye el proyecto, cómo se lee
el código y qué concepto de arquitectura hay detrás. Se recomienda leerlos en
este orden.

| Documento | Qué contiene |
| --- | --- |
| [00-como-usar-el-makefile.md](00-como-usar-el-makefile.md) | Qué es `make` y el Makefile; cómo compilar, generar el dataset de prueba y limpiar sin perder datos. |
| [01-guia-de-lectura-del-codigo.md](01-guia-de-lectura-del-codigo.md) | Qué es un header, por qué existen `types.h`, `keys.h` y `env.h`, y en qué orden leer los 6 archivos fuente con diagramas de dependencias y de flujo de datos. |
| [02-que-es-lmdb-y-memoria-virtual.md](02-que-es-lmdb-y-memoria-virtual.md) | Base de datos clave-valor, LMDB, `mmap`, caché de páginas, fallos de página mayor/menor, TLB y la conexión con el capítulo 5.4 de Patterson y Hennessy. |
| [03-lmdb-en-este-proyecto.md](03-lmdb-en-este-proyecto.md) | Las 5 sub-bases, el esquema de claves byte a byte, los 3 binarios, la técnica de caché fría, la medición de fallos de página y la lectura de negocio. |

## Ruta rápida para la defensa

1. Leer [00-como-usar-el-makefile.md](00-como-usar-el-makefile.md) para poder
   compilar y regenerar el dataset frente al jurado.
2. Estudiar [01-guia-de-lectura-del-codigo.md](01-guia-de-lectura-del-codigo.md)
   y abrir el código en el orden que propone.
3. Repasar [02-que-es-lmdb-y-memoria-virtual.md](02-que-es-lmdb-y-memoria-virtual.md)
   para tener los conceptos teóricos firmes.
4. Cerrar con [03-lmdb-en-este-proyecto.md](03-lmdb-en-este-proyecto.md), que
   ata la teoría con las decisiones concretas del código y los resultados
   medidos.

> Nota: todos los fragmentos de código y los nombres de funciones citados en
> estos documentos fueron verificados leyendo los archivos reales de `src/`.
