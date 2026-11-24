root /usr/share/nginx/html;
access_log off;
proxy_http_version 1.1;
proxy_read_timeout 3600s;

location / {
    try_files $uri /index.html;
}

location = /radar {
    proxy_set_header Upgrade $http_upgrade;
    proxy_set_header Connection $elevsafe_connection;
    proxy_set_header Authorization "";
    proxy_set_header X-Authenticated-User $elevsafe_user;
    proxy_set_header X-Forwarded-Proto $scheme;
    proxy_set_header X-Elevsafe-Proxy $elevsafe_proxy;
    proxy_pass http://relay:8090;
}

location = /ingest {
    auth_basic off;
    # Upstream errors can include the ingest query string.
    error_log /dev/null;
    proxy_set_header Upgrade $http_upgrade;
    proxy_set_header Connection $elevsafe_connection;
    proxy_set_header Authorization "";
    proxy_pass http://relay:8090;
}

location = /control-link {
    auth_basic off;
    proxy_set_header Upgrade $http_upgrade;
    proxy_set_header Connection $elevsafe_connection;
    proxy_set_header X-Authenticated-User "";
    proxy_set_header X-Forwarded-Proto $scheme;
    proxy_set_header X-Elevsafe-Proxy $elevsafe_proxy;
    proxy_set_header Authorization $http_authorization;
    proxy_pass http://relay:8090;
}

location ^~ /control { return 404; }
