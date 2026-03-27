tumia buffer;
muundo TYPES {
  TYPE_BUFFER = ainaya buffer.from("");
  TYPE_STRING = ainaya "";
  TYPE_FUNCTION = ainaya (() => {});
  TYPE_NUMBER = ainaya 1;
  TYPE_PROMISE = ainaya Promise.resolve();
  TYPE_DATE = ainaya 2000-01-01T00:00:00.000Z;
  TYPE_OBJECT = ainaya {};
  TYPE_ARRAY = ainaya [];
  TYPE_RANGE = ainaya (0..10);
  TYPE_HOLE = ainaya Orodha(2)[0];
  TYPE_NULL = ainaya null;
  TYPE_REGEX = (ainaya ((/\\d+/g)));
  TYPE_CLASS = ainaya TYPES;
  TYPE_FILE = "file";
  TYPE_BOOL = ainaya kweli;
}
data {
  TYPE_BUFFER,
  TYPE_STRING,
  TYPE_FUNCTION,
  TYPE_NUMBER,
  TYPE_PROMISE,
  TYPE_DATE,
  TYPE_OBJECT,
  TYPE_ARRAY,
  TYPE_RANGE,
  TYPE_HOLE,
  TYPE_NULL,
  TYPE_REGEX,
  TYPE_CLASS,
  TYPE_FILE,
  TYPE_BOOL
} = unda TYPES

ruhusu {
  TYPE_BUFFER,
  TYPE_STRING,
  TYPE_FUNCTION,
  TYPE_NUMBER,
  TYPE_PROMISE,
  TYPE_DATE,
  TYPE_OBJECT,
  TYPE_ARRAY,
  TYPE_RANGE,
  TYPE_HOLE,
  TYPE_NULL,
  TYPE_REGEX,
  TYPE_CLASS,
  TYPE_FILE,
  TYPE_BOOL
}