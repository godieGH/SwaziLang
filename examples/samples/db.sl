
muundo Db:
  &db_name;
  &tables = {};
  Db(db_name):
    $.db_name = db_name;
  
  tabia add_table(tb_obj):
    $.tables[tb_obj.tb_name] = tb_obj
  &tabia delete_db:
    //
  &tabia rename_db new_name:
    $.db_name = new_name
  
  &tabia delete_tb(tb_inst):
    $.tables[tb_inst.tb_name] = null
  
muundo Table:
  &rows = [];
  Table(db_instance, tb_name, ...cols):
    $.db_instance = db_instance;
    $.tb_name = tb_name;
    $.cols = cols;
    
    $.db_instance.add_table($)
  
  &tabia create(...dt):
    data rowOb = {}
    kwa kila col,i katika $.cols:
      rowOb[col] = dt[i]
    $.rows.ongeza(rowOb)
  &tabia read(id):
    rudisha $.rows.tafuta(r => r.id == id)
  &tabia update(id, ob):
    data entry = $.rows.tafuta(r => r.id == id);
    data nobj = {
      ...entry,
      ...ob
    }
    $.rows.ondoa(entry)
    $.rows.ongeza(nobj)
  &tabia delete(id):
    $.rows.ondoa($.rows.tafuta(r => r.id == id))

data app = unda Db("app");

data user_table = unda Table(app, "users", "id", "name", "married")
data post_table = unda Table(app, "posts", "id", "content", "like_counts")


user_table.create(3, "John Doe", kweli)
user_table.create(5, "Jane Doe", sikweli)

post_table.create(1, "Hellk world", 400)

# user_table.delete(3)
# chapisha user_table.read(4)
user_table.update(3, {
  name: "Hans Doe",
  married: sikweli
})

# app.delete_tb(user_table)
app.rename_db("app_db")

chapisha app