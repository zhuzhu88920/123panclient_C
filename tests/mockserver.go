// 本地协议 mock：复刻 login -> list -> download_info -> 302/href -> CDN Range 链路。
// 仅供 tests/ 使用，不参与发布产物。
package main

import (
	"bytes"
	"fmt"
	"net/http"
	"net/url"
	"os"
	"strings"
	"sync/atomic"
)

var testBytes = bytes.Repeat([]byte("0123456789abcdef"), 640) // 10240 bytes

func digits(s string) bool {
	if s == "" {
		return false
	}
	for _, c := range s {
		if c < '0' || c > '9' {
			return false
		}
	}
	return true
}

func hasSign(q url.Values) bool {
	for k, vs := range q {
		if digits(k) && len(vs) == 1 && strings.Count(vs[0], "-") == 2 {
			return true
		}
	}
	return false
}

func shapeOK(r *http.Request, needAuth bool) bool {
	if r.Header.Get("User-Agent") == "" || r.Header.Get("loginuuid") == "" ||
		r.Header.Get("platform") != "android" || r.Header.Get("x-app-version") != "2.4.0" {
		return false
	}
	if needAuth && !strings.HasPrefix(r.Header.Get("Authorization"), "Bearer ") {
		return false
	}
	return true
}

func badShape(w http.ResponseWriter) {
	fmt.Fprint(w, `{"code":9999,"message":"bad request shape or missing sign"}`)
}

func main() {
	port := "18080"
	if len(os.Args) > 1 {
		port = os.Args[1]
	}
	base := "http://127.0.0.1:" + port
	var firstHits int32
	var srv *http.Server
	mux := http.NewServeMux()

	mux.HandleFunc("/ping", func(w http.ResponseWriter, r *http.Request) {
		fmt.Fprint(w, "ok")
	})
	mux.HandleFunc("/quit", func(w http.ResponseWriter, r *http.Request) {
		fmt.Fprint(w, "bye")
	})
	mux.HandleFunc("/shutdown", func(w http.ResponseWriter, r *http.Request) {
		fmt.Fprint(w, "bye")
		go srv.Close()
	})
	mux.HandleFunc("/api/user/sign_in", func(w http.ResponseWriter, r *http.Request) {
		if !shapeOK(r, false) {
			badShape(w)
			return
		}
		fmt.Fprint(w, `{"code":200,"message":"success","data":{"login_type":1,"token":"mock-token-abcdef","expire":0}}`)
	})
	mux.HandleFunc("/b/api/file/list/new", func(w http.ResponseWriter, r *http.Request) {
		if !shapeOK(r, true) || !hasSign(r.URL.Query()) {
			badShape(w)
			return
		}
		switch r.URL.Query().Get("parentFileId") {
		case "0":
			fmt.Fprint(w, `{"code":0,"data":{"Total":2,"InfoList":[`+
				`{"FileName":"sub","Type":1,"Size":0,"FileId":"100","Etag":"","S3KeyFlag":0},`+
				`{"FileName":"file.bin","Type":0,"Size":10240,"FileId":200,"Etag":"e1","S3KeyFlag":"0"}`+
				`]}}`)
		case "100":
			fmt.Fprintf(w, `{"code":0,"data":{"Total":1,"InfoList":[{"FileName":"inner.bin","Type":0,"Size":%d,"FileId":201,"Etag":"e2","S3KeyFlag":"0"}]}}`, len(testBytes))
		default:
			fmt.Fprint(w, `{"code":0,"data":{"Total":0,"InfoList":[]}}`)
		}
	})
	mux.HandleFunc("/a/api/file/download_info", func(w http.ResponseWriter, r *http.Request) {
		if !shapeOK(r, true) {
			badShape(w)
			return
		}
		fmt.Fprintf(w, `{"code":0,"data":{"DownloadUrl":%q}}`, base+"/first")
	})
	mux.HandleFunc("/a/api/file/batch_download_info", func(w http.ResponseWriter, r *http.Request) {
		if !shapeOK(r, true) {
			badShape(w)
			return
		}
		fmt.Fprintf(w, `{"code":0,"data":{"DownloadUrl":%q}}`, base+"/first")
	})
	mux.HandleFunc("/a/api/file/upload_request", func(w http.ResponseWriter, r *http.Request) {
		fmt.Fprint(w, `{"code":0,"data":{}}`)
	})
	mux.HandleFunc("/a/api/file/trash", func(w http.ResponseWriter, r *http.Request) {
		fmt.Fprint(w, `{"code":0,"data":{}}`)
	})
	mux.HandleFunc("/a/api/share/create", func(w http.ResponseWriter, r *http.Request) {
		fmt.Fprint(w, `{"code":0,"data":{"ShareKey":"MOCKKEY"}}`)
	})
	mux.HandleFunc("/first", func(w http.ResponseWriter, r *http.Request) {
		if atomic.AddInt32(&firstHits, 1) == 1 {
			w.Header().Set("Location", base+"/cdn")
			w.WriteHeader(http.StatusFound)
			fmt.Fprintf(w, `<html><a href='%s/cdn'>download</a></html>`, base)
		} else {
			// 第二次故意不给 Location，验证 href 结构化扫描兜底
			fmt.Fprintf(w, `<html><a HREF = "%s/cdn" >download</a></html>`, base)
		}
	})
	mux.HandleFunc("/cdn", func(w http.ResponseWriter, r *http.Request) {
		rng := r.Header.Get("Range")
		start := 0
		if strings.HasPrefix(rng, "bytes=") {
			fmt.Sscanf(rng, "bytes=%d-", &start)
		}
		if start > 0 && start < len(testBytes) {
			w.Header().Set("Content-Range", fmt.Sprintf("bytes %d-%d/%d", start, len(testBytes)-1, len(testBytes)))
			w.WriteHeader(http.StatusPartialContent)
			w.Write(testBytes[start:])
			return
		}
		w.Write(testBytes)
	})

	srv = &http.Server{Addr: "127.0.0.1:" + port, Handler: mux}
	if err := srv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
