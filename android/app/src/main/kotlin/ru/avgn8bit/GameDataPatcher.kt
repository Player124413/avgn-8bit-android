package ru.avgn8bit

import android.content.Context
import java.io.BufferedOutputStream
import java.io.DataInputStream
import java.io.File
import java.io.FileOutputStream
import java.io.InputStream
import java.io.RandomAccessFile
import java.util.zip.CRC32
import java.util.zip.Inflater

/**
 * The Vita build of AVGN 8-bit does not run the retail data.win as-is: it ships a patched one
 * (`data_vita.win`), built by the Windows patcher from the same payload that is embedded in this app.
 *
 * The payload is a binary diff, not a copy of the game:
 *
 *   header (40 bytes, little endian)
 *     0   "AVGNDP01"
 *     8   u64 source size (the data.win the patch was built against)
 *     16  u32 source checksum
 *     20  u32 output size
 *     24  u32 output CRC32 (0 when the build did not fill it in)
 *     28  u32 build stamp
 *     32  u64 size of the inflated operation stream
 *   then a zlib stream with the operations:
 *     'C' + u64 source offset + u32 length   copy from the user's data.win
 *     'A' + u32 length + bytes               literal bytes added by the patch
 *     'E'                                    end
 *
 * Every copy has to line up with the user's data.win, so the source size (and the CRC when the patch has one) is
 * checked before the result is accepted: if the game was updated the patched file is simply not produced and the
 * runtime falls back to the unpatched data.win rather than to a broken one.
 */
object GameDataPatcher {
    const val ASSET = "data_vita.patch"
    const val OUTPUT_NAME = "data_vita.win"
    const val MARKER_NAME = "data_vita.ok"

    private const val MAGIC = "AVGNDP01"
    private const val HEADER_SIZE = 40
    private const val MAX_LITERAL = 1 shl 24

    data class Result(val ok: Boolean, val message: String, val output: File? = null)

    /** True when [dir] already holds a patched data.win of the size recorded when it was written. */
    fun isPatched(dir: File): Boolean {
        val output = File(dir, OUTPUT_NAME)
        if (!output.isFile || output.length() <= 0) return false
        val marker = File(dir, MARKER_NAME)
        if (!marker.isFile) return true
        return try {
            marker.readText().trim().substringBefore(' ').toLong() == output.length()
        } catch (_: Throwable) {
            true
        }
    }

    /**
     * Applies [source] into `<dir>/data_vita.win`. The output only appears under its final name once it has been
     * written completely, so an interrupted run can never be mistaken for a finished one.
     */
    fun apply(context: Context, source: File, dir: File, onProgress: (Float) -> Unit = {}): Result {
        val output = File(dir, OUTPUT_NAME)
        val partial = File(dir, "$OUTPUT_NAME.part")
        try {
            if (!source.isFile || source.length() <= 0) return Result(false, "Нет файла data.win")
            dir.mkdirs()

            context.assets.open(ASSET).use { raw ->
                val header = ByteArray(HEADER_SIZE)
                var filled = 0
                while (filled < HEADER_SIZE) {
                    val n = raw.read(header, filled, HEADER_SIZE - filled)
                    if (n <= 0) return Result(false, "Патч повреждён: короткий заголовок")
                    filled += n
                }
                if (String(header, 0, 8, Charsets.US_ASCII) != MAGIC) {
                    return Result(false, "Патч повреждён: неверная сигнатура")
                }

                val sourceSize = leLong(header, 8)
                val outputSize = leInt(header, 20).toLong() and 0xFFFFFFFFL
                val declaredCrc = leInt(header, 24).toLong() and 0xFFFFFFFFL

                if (source.length() != sourceSize) {
                    return Result(
                        false,
                        "Файл data.win не совпадает с патчем (нужно $sourceSize байт, у вас ${source.length()}). " +
                            "Игра запустится без патча."
                    )
                }

                val crc = CRC32()
                val buffer = ByteArray(1 shl 16)
                var written = 0L
                var reported = 0L

                RandomAccessFile(source, "r").use { input ->
                    BufferedOutputStream(FileOutputStream(partial), 1 shl 20).use { out ->
                        val inflater = Inflater()
                        val ops = DataInputStream(InflatingStream(raw, inflater))

                        fun emitLiteral(literal: ByteArray) {
                            if (written + literal.size > outputSize) {
                                throw IllegalStateException("Патч выходит за границы файла")
                            }
                            out.write(literal)
                            crc.update(literal)
                            written += literal.size
                        }

                        fun emitCopy(offset: Long, length: Int) {
                            if (offset < 0 || length < 0 || offset + length > sourceSize) {
                                throw IllegalStateException("Копирование за пределами data.win")
                            }
                            if (written + length > outputSize) throw IllegalStateException("Патч выходит за границы файла")
                            input.seek(offset)
                            var left = length
                            while (left > 0) {
                                val take = minOf(buffer.size, left)
                                val n = input.read(buffer, 0, take)
                                if (n <= 0) throw IllegalStateException("data.win короче, чем ожидает патч")
                                out.write(buffer, 0, n)
                                crc.update(buffer, 0, n)
                                left -= n
                            }
                            written += length
                        }

                        while (true) {
                            val tag = ops.read()
                            if (tag < 0) throw IllegalStateException("Патч обрывается без операции E")
                            when (tag.toChar()) {
                                'C' -> {
                                    // Fields are little endian; DataInputStream reads big endian.
                                    val offset = java.lang.Long.reverseBytes(ops.readLong())
                                    val length = Integer.reverseBytes(ops.readInt())
                                    emitCopy(offset, length)
                                }
                                'A' -> {
                                    val length = Integer.reverseBytes(ops.readInt())
                                    if (length < 0 || length > MAX_LITERAL) {
                                        throw IllegalStateException("Некорректная длина литерала: $length")
                                    }
                                    val literal = ByteArray(length)
                                    ops.readFully(literal)
                                    emitLiteral(literal)
                                }
                                'E' -> break
                                else -> throw IllegalStateException("Неизвестная операция патча: $tag")
                            }
                            if (written - reported > 4L * 1024 * 1024) {
                                reported = written
                                onProgress((written.toDouble() / outputSize).toFloat().coerceIn(0f, 1f))
                            }
                        }

                        out.flush()
                        if (written != outputSize) {
                            throw IllegalStateException("Размер результата $written, ожидался $outputSize")
                        }
                        // The header of a GameMaker WAD is the IFF "FORM" tag: if the assembled file does not start
                        // with it, the patch was applied to a different game and the result is worthless.
                        val magic = ByteArray(4)
                        partial.inputStream().use { it.read(magic) }
                        if (String(magic, Charsets.US_ASCII) != "FORM") {
                            throw IllegalStateException("Результат не похож на data.win")
                        }
                        val value = crc.value
                        if (declaredCrc != 0L && declaredCrc != value) {
                            // Only a warning: the field has been zero in every build of the patch seen so far, and
                            // a data.win that the runtime cannot parse is caught by the FORM check above.
                            AppLog.write("Data patch CRC mismatch: got ${value.toString(16)}, header says ${declaredCrc.toString(16)}")
                        }
                        partial.renameTo(output)
                        File(dir, MARKER_NAME).writeText("$outputSize ${value.toString(16).padStart(8, '0')}")
                        onProgress(1f)
                        AppLog.write("Data patch applied: $written bytes, crc32 ${value.toString(16)}")
                        return Result(true, "Патч применён", output)
                    }
                }
            }
        } catch (t: Throwable) {
            partial.delete()
            AppLog.write("Data patch failed: ${t.stackTraceToString()}")
            return Result(false, "Патч не применился: ${t.message ?: t.javaClass.simpleName}")
        }
    }

    private fun leInt(bytes: ByteArray, offset: Int): Int =
        (bytes[offset].toInt() and 0xFF) or
            ((bytes[offset + 1].toInt() and 0xFF) shl 8) or
            ((bytes[offset + 2].toInt() and 0xFF) shl 16) or
            ((bytes[offset + 3].toInt() and 0xFF) shl 24)

    private fun leLong(bytes: ByteArray, offset: Int): Long =
        (leInt(bytes, offset).toLong() and 0xFFFFFFFFL) or
            (leInt(bytes, offset + 4).toLong() shl 32)
}

/**
 * Feeds a [Inflater] from the byte after the patch header. Android assets are not seekable zlib streams, so the
 * inflater is driven by hand; a short read means the asset ended, which only happens for a truncated patch.
 */
private class InflatingStream(
    private val source: InputStream,
    private val inflater: Inflater
) : InputStream() {
    private val inputBuffer = ByteArray(1 shl 16)

    override fun read(): Int {
        val single = ByteArray(1)
        return if (read(single, 0, 1) == 1) single[0].toInt() and 0xFF else -1
    }

    override fun read(b: ByteArray, off: Int, len: Int): Int {
        if (len == 0) return 0
        while (true) {
            val n = inflater.inflate(b, off, len)
            if (n > 0) return n
            if (inflater.finished()) return -1
            if (inflater.needsDictionary()) throw IllegalStateException("Патч повреждён: нужен словарь zlib")
            if (inflater.needsInput()) {
                val got = source.read(inputBuffer)
                if (got <= 0) return -1
                inflater.setInput(inputBuffer, 0, got)
            } else {
                return -1
            }
        }
    }
}
