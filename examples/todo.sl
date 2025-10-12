# Todo List Project

tumia regex kutoka "regex"

data todos = [];

data usg = `
Usage commands: \n
  >>> list — To list all todos
  >>> add — To add new todos(open the prompt)
  >>> remove <id> — To remove a todo (with a specified id)
  ${"_".rudia(40)} \n
  >>> quit — turn off the todo app
  >>> help / h — to see this help again
  
`
chapisha usg
chapisha "Start todoing..."

data TODO_CUR_ID = 1;
kazi add:
  id = TODO_CUR_ID;
  title = soma(">>> Weka title: ");
  desc = soma(">>> Weka description: ");
  
  todos.ongeza({id, title, desc});
  TODO_CUR_ID++
  //add ended here

kazi listing:
  kwa kila val,i katika todos:
    chapisha `${Neno(i + 1)}) id: ${Neno(val.id)}, ${val.title} \n => .... ${val.desc}`

kazi namba_sahihi n :
   kama n.nineno :
      kama regex.match(n, "\\D") { 
         rudisha sikweli
      }
      rudisha Namba(n).ninamba na !Namba(n).siSahihi
   vinginevyo:
      fanya {
         jaribu:
            rudisha n.ninamba na !n.siSahihi
         makosa err:
            rudisha sikweli
      }
kazi removing line:
  data cmd_arr = line.orodhesha(" ");
  data [cmd, ...ids] = cmd_arr;
  kama ids.idadi == 0:
    chapisha "You should use id(s) after rm/remove command"
    rudisha
  #chapisha ids
  kwa kila val,i katika ids:
    kama !namba_sahihi(val) :
      chapisha "Invalid Id used, please use numeric id eg. remove 1 2 3"
      rudisha;
    data tdi = todos.tafutaIndex(e => e.id == val)
    kama todos.ondoa(todos[tdi]) :
      chapisha `Todo yenye id ${val} imeondolowa kwenye orodha.`
      endelea;
    vinginevyo:
      chapisha `Huwezi kufuta todo id(${val}), haipo kwenye orodha`;
      endelea;
  rudisha;
    
wakati kweli :
  data line = soma(`${todos.idadi>0?Neno(todos.idadi):">"}>> `).sawazisha()
  kama line sawa "help" au line sawa "h":
    chapisha usg;
    endelea;
  # handle todo listing
  kama line sawa "list" au line sawa "l" au line sawa "ls":
    listing();
    endelea;
  # handle adding todos
  kama line sawa "add":
    add();
    chapisha "Todo added successifully..."
    endelea;
  #handle removing todos
  kama line.orodhesha(" ").kuna("remove") au line.orodhesha(" ").kuna("rm"):
    removing(line)
    endelea;
  
  kama line sawa "quit" au line sawa "q":
    chapisha "quiting..."
    simama;
  
  kama line sawa "":
    endelea;
  
  chapisha "Command not found";
  // end of while loop