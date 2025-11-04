muundo Point {
  &a;&b;
  Point(a,b) =>> self.a=a;self.b=b;
  tabia getPoint() =>> rudisha `(${self.x},${self.y})`
  
  tabia get_circle_radius(x, y) {
    data rudius = ((x - self.a)**2 + (y - self.b)**2)**(0.5)
    rudisha rudius;
  }
}
muundo Circle(Point) {
  *PI = 3.14;
  Circle(center, radius) {
    data [a,b] = center;
    $.radius = radius;
    super(a,b)
  }
  tabia calcArea() {
    rudisha Circle.PI * self.radius * self.radius;
  }
  
  # calc circumference
  tabia calcCircum() {
    rudisha 2 * Circle.PI * self.radius;
  }
  
  tabia thabiti get_circle_points {
    data a = self.a;
    data b = self.b;
    data r = self.radius;
    rudisha ({
      tabia x(y) {
        rudisha (r.pow() - (y - b).pow()).pow(0.5) - a;
      }
      tabia y(x) {
        rudisha (r.pow() - (x - a).pow()).root() - b
      }
    })
  }
  
  tabia draw() {
      data r = self.radius;
      data yScale = 0.5;
  
      data sizeX = (r*2).kadiria() + 1;
      data sizeY = (r*2*yScale).kadiria() + 1;
  
      kwa(y = 0; y <= sizeY; y++):
        kwa(x = 0; x <= sizeX; x++):
          data dx = x - r;
          data dy = (y / yScale) - r;
          data dist = (dx.pow() + dy.pow()).pow(0.5);
  
          # thinner outline, just print * near the radius
          kama (dist >= r - 0.3 && dist <= r + 0.3):
            andika "*";
          vinginevyo:
            andika " ";
        andika "\n";
  }
  
}

data cr = unda Circle([0, 0], 12)

#chapisha cr.get_circle_points.x(4)
#chapisha cr.get_circle_points.y(30) // why this returns nan, because 30 is out of range of on that circle dimensions to be a point
#cr.draw()

muundo Rect(Point) {
  length = 0;width = 0;
  Rect(point, length, width) {
    super(point.a, point.b)
    self.length = length;
    self.width = width;
    
  }
  @tabia getPoint() {} // override and hide
  @tabia get_circle_radius() {} // override and hide
  
  &tabia thabiti get_starting_point {
    rudisha `A(${self.a}, ${self.b})`;
  }
  &tabia thabiti get_point_B {
    rudisha `B(${self.a + self.length}, ${self.b})`;
  }
  &tabia thabiti get_point_C {
    rudisha `C(${self.a + self.length}, ${self.b + self.width})`;
  }
  &tabia thabiti get_point_D {
    rudisha `D(${self.a}, ${self.b + self.width})`;
  }
  
  &tabia calc_area() {
    rudisha self.length * self.width;
  }
  &tabia calc_circum() {
    rudisha (self.length + self.width)*2;
  }
  
  @tabia point_A() {
    rudisha [self.a, self.b]
  }
  @tabia point_B() {
    rudisha [self.a + self.length, self.b]
  }
  @tabia point_C() {
    rudisha [self.a + self.length, self.b + self.width]
  }
  @tabia point_D() {
    rudisha [self.a, self.b + self.width]
  }
  tabia draw() {
    andika "*"; andika "—".rudia(self.length); andika "*";
    kwa(i=1;i<=(self.width / 2 ).kadiria();i++):
      andika "\n|"; andika " ".rudia(self.length); andika "|";
      kama i >= (self.width / 2).kadiria() {
        andika "\n"
      }
    andika "*"; andika "—".rudia(self.length); andika "*\n";
    
    chapisha "Area: " + self.calc_area();
    chapisha "Circumference: " + self.calc_circum();
    chapisha "Points: " + self.get_starting_point + ", " + self.get_point_B + ", " + self.get_point_C + ", " + self.get_point_D;
  }
}

data rect = unda Rect(unda Point(5,8), 60, 60)
#chapisha rect.get_starting_point
#chapisha rect.get_point_B
#chapisha rect.get_point_C
#chapisha rect.get_point_D

#chapisha rect.calc_area()
#chapisha rect.calc_circum()

rect.draw() 