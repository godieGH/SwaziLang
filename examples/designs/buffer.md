---

📦 buffer Module API

Creation

`buffer.alloc(size)`

`buffer.from(arrayOfBytes)`

`buffer.from(string, encoding?)`

`buffer.fromBase64(base64String)`

`buffer.isBuffer(buf)` -> Bool

`buf.size` -> this will be implemented out of the module basically in expressionEval.cpp in mem branch just return the data.size() of the uint_8 vector or anyway we'll do


---

Byte Access

buffer.readUInt8(buf, offset)

buffer.writeUInt8(buf, offset, value)



---

Multi-Byte Numbers

buffer.readUInt16LE(buf, offset)

buffer.writeUInt16LE(buf, offset, value)

buffer.readUInt32BE(buf, offset)

buffer.writeUInt32BE(buf, offset, value)



---

Floating Point

buffer.readFloat32(buf, offset)

buffer.writeFloat32(buf, offset, value)



---

Manipulation

buffer.slice(buf, start, end?)

buffer.concat([buf1, buf2, ...])



---

Encoding

buffer.toString(buf, encoding="utf8")

buffer.toBase64(buf)



---

Decoding

buffer.from(string, encoding="utf8")

buffer.fromBase64(base64String)


---