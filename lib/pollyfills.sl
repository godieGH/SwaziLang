tumia timers, fs;
timers.setImmediate = (fn) => {
  rudisha timers.setTimeout(0, fn)
}
fs.mkdir = (pth, opt?) => {
  data recursive = opt?.recursive ?? sikweli;

  jaribu {
    fs.makeDir(pth, recursive)
    rudisha kweli
  } makosa e {
    rudisha sikweli
  }
}
fs.readdir = (dir_path, opt?) => {
  data dir_path = path.resolve(dir_path)
  data options = {
    recursive: opt?.recursive au sikweli,
    withFileTypes: opt?.withFileTypes au sikweli,
    absolute: opt?.absolute au sikweli,
    filter: opt?.filter au null,
    ignore: opt?.ignore au [],
    sort: opt?.sort au null
  }

  data stack = [dir_path]
  data results = []

  wakati !stack.empty() {
    data current = stack.pop()

    jaribu {
      data entries = fs.listDir(current)

      kwa kila entry ktk entries {
        kama options.ignore.kuna(entry) {
          endelea
        }

        data full_path = path.resolve(current, entry)
        data relative_path = path.relative(dir_path, full_path)

        data stat = fs.lstat(full_path)
        data is_dir = stat.isDir

        kama options.filter na !options.filter(entry) {
          endelea
        }

        kama options.withFileTypes {
          results.push( {
            name: options.absolute ? full_path : relative_path,
            type: is_dir ? "directory" : (stat.isSymlink ? "symlink" : "file"),
            size: stat.size
          })
        } sivyo {
          results.push(options.absolute ? full_path : relative_path)
        }

        kama options.recursive na is_dir {
          stack.push(full_path)
        }
      }
    } makosa err {
      chapisha("Error reading directory " + current + ": " + err)
    }
  }

  data type_rank = (t) => {
    kama t == "directory" =>> rudisha 0
    kama t == "file" =>> rudisha 1
    kama t == "symlink" =>> rudisha 2
    rudisha 3
  }
  data compare_type = (a, b) => {
    data ra = type_rank(a.type)
    data rb = type_rank(b.type)
    kama ra < rb =>> rudisha -1
    kama ra > rb =>> rudisha 1
    rudisha 0
  }
  data compare_name = (a, b) => {
    data i = 0
    data len_a = a.size
    data len_b = b.size
    data min = len_a < len_b ? len_a : len_b
    wakati i < min {
      kama a[i] < b[i] =>> rudisha -1
      kama a[i] > b[i] =>> rudisha 1
      i = i + 1
    }
    kama len_a < len_b =>> rudisha -1
    kama len_a > len_b =>> rudisha 1
    rudisha 0
  }
  data compare_size = (a, b) => {
    kama a.size < b.size =>> rudisha -1
    kama a.size > b.size =>> rudisha 1
    rudisha 0
  }

  kama options.sort == "name" {
    results = results.sort((a, b) => {
      data a_name = options.withFileTypes ? a.name : a
      data b_name = options.withFileTypes ? b.name : b
      rudisha compare_name(a_name.toLower(), b_name.toLower())
    })
  } sivyo kama options.sort == "type" {
    kama options.withFileTypes {
      results = results.sort((a, b) => compare_type(a, b))
    }
  } sivyo kama options.sort == "size" {
    kama options.withFileTypes {
      results = results.sort((a, b) => compare_size(a, b))
    }
  }

  rudisha results
}