
tumia {parse kama tomlParse} kutoka "toml"
tumia fs kutoka "fs"

data tomlstr = "
[data]
name = \"John Doe\"
age = 12
is_married = true
"
chapisha tomlParse(tomlstr)

# fetch from file
data file = fs.readFile("toml_str.toml")

#chapisha tomlParse(file)