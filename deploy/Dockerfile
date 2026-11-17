# Optional: a ~1 MB image holding ONLY your app's file (no operating system inside).
#   make APP=ride release && cp build/ride/ride deploy/app && cd deploy
#   docker build -t ride . && docker run -p 8080:8080 ride
FROM scratch
COPY app /app
EXPOSE 8080
WORKDIR /data
ENTRYPOINT ["/app"]
