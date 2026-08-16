/* Force-included (-include) ahead of every nano/gnulib source file -
 * same role coreutils/poc/poc_prelude.h and bash/poc/bash_prelude.h
 * play for their own ports. Starts empty and picks up real gaps as
 * the build discovers them (musl's real headers are used throughout,
 * not gnulib's own lib/ wrapper headers - see poc_prelude.h's own
 * comment for why that's safe: this build never runs gnulib's
 * generation step, so those wrapper files are simply inert).
 */
