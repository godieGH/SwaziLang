/*
* This is swazilang custom tool to parse tomil to swazilang objects
*/
tumia fs kutoka "fs"
tumia reg kutoka "regex"
tumia "json"

data toml_str = fs.readFile("toml_str.toml")
data tokens = [];

i = 0;
wakati kweli:
  // if end of line stop the loop
  kama i > toml_str.herufi {simama}
  
  // ignore white spaces
  kama toml_str.herufiYa(i) === " " || toml_str.herufiYa(i)  === "\t":
    i++;
    endelea;
  
  
  // ignore comments
  kama toml_str.herufiYa(i) === "#":
    wakati i < toml_str.herufi && toml_str.herufiYa(i) !== "\n":
      i++
      endelea;
  
  kama toml_str.herufiYa(i) === "[" && toml_str.herufiYa(i+1) === "[" :
    tokens.ongeza({type:"ATS", value:"[["})
    i++;i++;
    endelea;
  kama toml_str.herufiYa(i) === "]" && toml_str.herufiYa(i+1) === "]" :
    tokens.ongeza({type:"ATE", value:"]]"})
    i++;i++;
    endelea;
  
  // single char tokens
  chagua toml_str.herufiYa(i):
    ikiwa "=":
      fanya:
        tokens.ongeza({type:"EQ",value:toml_str.herufiYa(i)})
        i++
        endelea;
      simama;
    ikiwa "[":
      fanya:
        tokens.ongeza({type:"TS",value:"["})
        i++;
        endelea;
      simama;
    ikiwa "]":
      fanya:
        tokens.ongeza({type:"TE",value:"]"})
        i++;
        endelea;
      simama;
    ikiwa ".":
      fanya:
        tokens.ongeza({type:"PR",value:"."})
        i++;
        endelea;
      simama;
    ikiwa ",":
      fanya:
        tokens.ongeza({type:"CM",value:","})
        i++;
        endelea;
      simama;
  
  // multi-char
  kama toml_str.herufiYa(i) === "\"" :
    i++ // skip opening quote "\""
    str_val = "";
    fcq = sikweli
    
    // Loop until we find the closing quote or hit the end of the file/string
    wakati i < toml_str.herufi && toml_str.herufiYa(i) !== "\n":
      // 1. Check for the *closing* quote
      kama toml_str.herufiYa(i) === "\"" :
        tokens.ongeza({type:"STR",value:str_val});
        i++; // Consume the closing quote
        fcq = kweli;
        simama; // Jump to the outer loop to look for the next token
      
      // 2. Check for the *escape* character (backslash)
      kama toml_str.herufiYa(i) === "\\":
        i++; // Consume the backslash
        // Check what character is being escaped
        chagua toml_str.herufiYa(i){
          ikiwa "\""{
            str_val += "\""
            i++
            simama;
          }
          // You might add other escapes here, like \n, \t, \\
          ikiwa "\\" {
            str_val += "\\"
            i++
            simama;
          }
          ikiwa "n" {
            str_val += "\n"
            i++
            simama;
          }
          // For anything else, treat the sequence as literal (or throw an error)
          kaida {
            // For simplicity, treat as literal characters (e.g., "\a" is "\a")
            str_val += "\\"
            str_val += toml_str.herufiYa(i);
            i++
            simama;
          }
        }
        endelea;

      // 3. Normal character: just add and advance
      str_val += toml_str.herufiYa(i);
      i++
      
    kama !fcq:
      Makosa("No closing quote")
    endelea;
  kama reg.test(toml_str.herufiYa(i), "[-+0-9]"):
    kama toml_str.herufiYa(i) === "+" au toml_str.herufiYa(i) === '-':
      kama !reg.test(toml_str.herufiYa(i+1), "[0-9]") {
        Makosa("Invalid signed numver, there should be a valid number after +/-")
      }
      kama toml_str.herufi && reg.test(toml_str.herufiYa(i-1), "[0-9]" ):
        Makosa("Missuse or invalid signs")
    data num = toml_str.herufiYa(i);
    data is_dec = sikweli;
    i++
    
    wakati i < toml_str.herufi && toml_str.herufiYa(i) !== "\n":
      kama !reg.test(toml_str.herufiYa(i), "[0-9_.eE]") {simama}
      kama toml_str.herufiYa(i) === "." {
        kama is_dec {
          Makosa("Invalid float numbers")
        }
        kama !reg.test(toml_str.herufiYa(i+1), '[0-9]') {
          Makosa("Invalid decimal, no number after '.'")
        }
        num += toml_str.herufiYa(i)
        is_dec = kweli
        i++
        endelea
      }
      kama toml_str.herufiYa(i) === "_" {
        kama !reg.test(toml_str.herufiYa(i+1), '[0-9]') {
          Makosa('Using _ shouod only appy for digit separation, should be surrounded by numbers')
        }
        i++
        endelea
      }
      kama toml_str.herufiYa(i) === "e" au toml_str.herufiYa(i) === "E":
        kama !reg.test(toml_str.herufiYa(i-1), "[0-9]") {
          Makosa("Use e/E exponent after numbers only")
        }
        kama !reg.test(toml_str.herufiYa(i+1), "[0-9+-]") {
          Makosa(`Invalid character ${toml_str.herufiYa(i+1)} after exponent`)
        }
        num += toml_str.herufiYa(i);
        i++
        kama toml_str.herufiYa(i) === '+' au toml_str.herufiYa(i) === '-':
          num += toml_str.herufiYa(i)
          i++
        endelea;
        
      num += toml_str.herufiYa(i)
      i++;
    
    tokens.ongeza({type:"NUM", value:num})
    endelea;
  kama reg.test(toml_str.herufiYa(i), '[a-zA-Z_]'):
    data id_val = toml_str.herufiYa(i)
    i++
    
    wakati i < toml_str.herufi && toml_str.herufiYa(i) !== "\n":
      kama !reg.test(toml_str.herufiYa(i), "[a-zA-Z0-9_-]") {simama}
      id_val += toml_str.herufiYa(i)
      i++
    
    kama !["true","false"].kuna(id_val):
      tokens.ongeza({type:"ID", value:id_val})
    chagua id_val:
      ikiwa "true":
        fanya:
          tokens.ongeza({type:"BOOL", value:"true"})
        simama;
      ikiwa "false":
        fanya:
          tokens.ongeza({type:"BOOL", value:"false"})
        simama;
    endelea
  
  kama toml_str.herufiYa(i) === "\n" || toml_str.herufiYa(i)  === "":
    i++;
    endelea;
  Makosa(`Invalid ${toml_str.herufiYa(i)} character in toml`)

//parser
data obj = {}
data cur_header = obj;
data root = obj

kwa kila tok katika tokens:
  tok.expect = (t, msg="") => {
    kama tok.value !== t {Makosa(`Expected ${t} found ${tok.value}` + msg)}
  }

data cur = 0;
kazi parse_arrays {
  data arr = []
  wakati tokens[cur].value !== "]":
    kama tokens[cur].type === "CM":
      cur++
      endelea;
    arr.ongeza(parse_value())
  
  data s = sikweli
  s = arr.badili(elm => !(elm.aina === arr[0].aina)).kuna(kweli)
  kama s {Makosa("Arrays can only have same data types")}
  cur++
  rudisha arr
}
kazi parse_value {
  data val = tokens[cur].value
  
  kama tokens[cur].type === "TS":
    cur++
    rudisha parse_arrays()
  
  kama val === "true" {val=kweli;cur++;rudisha val}
  kama val === "false" {val=sikweli;cur++;rudisha val}
  kama tokens[cur].type sawa "NUM" {val=Namba(val);cur++;rudisha val}
  
  kama tokens[cur].type sawa "STR" au tokens[cur].type sawa "ID" {cur++; rudisha val}
  
  
  Makosa(`Invalid value ${val} after assignment`)
  
}

wakati kweli:
  kama cur >= tokens.idadi {simama}
  
  kama tokens[cur].type === "ID":
    key = tokens[cur].value
    cur++
    
    tokens[cur].expect("=", "Missing assign after Id")
    cur++
    
    cur_header[key] = parse_value()
    endelea;
  
  kama tokens[cur].type === "TS":
    cur++
    data cur_context = root
    
    data path = [];
    wakati tokens[cur].value !== "]":
      kama tokens[cur].type === "ID":
        path.ongeza(tokens[cur].value)
        cur++
        endelea
      kama tokens[cur].type === "PR":
        cur++
        kama tokens[cur].value === "]" {Makosa("cant use [a.] in toml");}
        endelea
      Makosa("Invalid token in table header")
      
    tokens[cur].expect("]", "Missing closing closing brackets")
    cur++
    
    data current_node = root
    kwa(i=0;i<(path.idadi-1);i++):
      data key = path[i]
      
      kama !Object.keys(current_node).kuna(key):
        current_node[key] = {}
      
      kama ainaya(current_node[key]) !== "object" {Makosa(`'${key}' (part of path) is not a table, cannot nest.`)}
      
      current_node = current_node[key]
    
    data parent_node = current_node
    data new_table_name = path[path.idadi - 1]
    kama Object.keys(parent_node).kuna(new_table_name) { Makosa(`Table '${new_table_name}' is already defined.`) }
    
    data new_table = {}
    parent_node[new_table_name] = new_table
    
    cur_header = new_table
    endelea;
  kama tokens[cur].type === "ATS":
    cur++
    
    data path = [];
    wakati tokens[cur].value !== "]]":
      kama tokens[cur].type sawa "ID":
        path.ongeza(tokens[cur].value)
        cur++
        endelea;
      kama tokens[cur].type sawa "PR":
        cur++
        kama tokens[cur].value sawa "]]" {Makosa("Path cannot end with a dot")}
        endelea;
      Makosa(`Invalid token '${tokens[cur].value}' used in array table header.`)
    
    tokens[cur].expect("]]", "Missing closing double ]]")
    cur++
    
    data current_node = root
    kwa(i=0;i<(path.idadi-1);i++):
      data key = path[i];
      
      kama !Object.keys(current_node).kuna(key):
        current_node[key] = {}
      
      kama ainaya(current_node[key]) !== "object" {Makosa(`'${key}' (part of path) is not a table, cannot nest.`)}
      current_node = current_node[key]
    
    data parent_node = current_node
    data array_key = path[path.idadi - 1]
    
    kama !Object.keys(parent_node).kuna(array_key):
      parent_node[array_key] = []
    
    kama !parent_node[array_key].niorodha {Makosa(`'${array_key}' is not an array, expected an array of tables.`)}
    
    data table_array = parent_node[array_key]
    
    data new_table_instance = {};
    table_array.ongeza(new_table_instance)
    
    cur_header = new_table_instance;
    endelea;
  
  cur++

chapisha obj