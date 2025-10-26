tumia http kutoka "http"


muundo Mtandao {
   Mtandao(url, opt=null) {
      $.url = url;
      $.opt = opt;
   }
   *&tabia tuma_req(url) {
      rudisha http.get(url)
   }
   @&tabia setOpt(opt) {
      $.opt = opt
   }
   tabia tumaReq {
      rudisha http.get($.url)
   }
}

data tuma_req = Mtandao.tuma_req

ruhusu {
   tuma_req,
   Mtandao
}