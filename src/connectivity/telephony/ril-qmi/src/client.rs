// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::errors::QmuxError,
    crate::transport::{ClientId, SvcId},
    crate::transport::{QmiResponse, QmiTransport},
    bytes::{BufMut, BytesMut},
    fidl::endpoints::ServerEnd,
    fidl_fuchsia_telephony_ril::NetworkConnectionMarker,
    fuchsia_syslog::macros::*,
    futures::lock::Mutex,
    parking_lot::RwLock,
    qmi_protocol::{Decodable, Encodable, QmiResult},
    std::collections::HashMap,
    std::fmt::Debug,
    std::marker::Unpin,
    std::ops::Deref,
    std::sync::Arc,
};

#[derive(Debug)]
pub struct ClientSvcMap(RwLock<HashMap<SvcId, ClientId>>);

impl Default for ClientSvcMap {
    fn default() -> Self {
        let mut m = HashMap::new();
        // Requests for IDs occur on the CTL service (ID 0),
        // this default mapping allows the client to request an ID
        // without a unique client just for this functionality
        m.insert(SvcId(0), ClientId(0));
        ClientSvcMap(RwLock::new(m))
    }
}
impl Deref for ClientSvcMap {
    type Target = RwLock<HashMap<SvcId, ClientId>>;
    fn deref(&self) -> &Self::Target {
        &self.0
    }
}

#[derive(Debug)]
pub struct Connection {
    pub conn: ServerEnd<NetworkConnectionMarker>,
    pub pkt_handle: u32,
}

#[derive(Debug)]
pub struct QmiClient {
    inner: Arc<QmiTransport>,
    clients: ClientSvcMap,
    data_conn: Mutex<Option<Connection>>,
}

impl Unpin for QmiClient {}

impl QmiClient {
    pub fn new(inner: Arc<QmiTransport>) -> Self {
        QmiClient { inner: inner, clients: ClientSvcMap::default(), data_conn: Mutex::new(None) }
    }

    /// Connect a data bearer to a qmi client.
    /// TODO(bwb): support multiple connections
    pub async fn set_data_connection(&self, conn: Connection) {
        let mut data_conn = self.data_conn.lock().await;
        *data_conn = Some(conn);
    }

    /// Send a QMI message and allocate the client IDs for the service
    /// if they have not yet been
    pub async fn send_msg<'a, E: Encodable + 'a>(
        &'a self,
        msg: E,
    ) -> Result<QmiResult<E::DecodeResult>, QmuxError>
    where
        <E as Encodable>::DecodeResult: Decodable + Debug,
    {
        let svc_id = SvcId(msg.svc_id());
        let mut need_id = false;
        {
            let map = self.clients.read();
            // allocate a client id for this service
            if map.get(&svc_id).is_none() {
                need_id = true;
            }
        }
        if need_id {
            use qmi_protocol::CTL::{GetClientIdReq, GetClientIdResp};
            fx_log_info!("allocating a client ID for service: {}", svc_id.0);
            let resp: QmiResult<GetClientIdResp> =
                self.send_msg_actual(GetClientIdReq::new(svc_id.0)).await?;
            let client_id_resp = resp.unwrap(); // TODO from trait for QmiError to QmuxError
            let mut map = self.clients.write();
            assert_eq!(client_id_resp.svc_type, svc_id.0);
            map.insert(svc_id, ClientId(client_id_resp.client_id));
        }
        Ok(self.send_msg_actual(msg).await?)
    }

    fn get_client_id(&self, svc_id: SvcId) -> ClientId {
        let clients = self.clients.read();
        *clients
            .get(&svc_id)
            .expect("Precondition of calling get_client_id is to have verified an ID is allocated")
    }

    /// Send a QMI message without checking if a client ID has been allocated for the service
    async fn send_msg_actual<'a, E: Encodable + 'a, D: Decodable + Debug>(
        &'a self,
        msg: E,
    ) -> Result<QmiResult<D>, QmuxError> {
        fx_log_info!("Sending a structured QMI message");

        let svc_id = SvcId(msg.svc_id());
        let client_id = self.get_client_id(svc_id);

        let tx_id = self.inner.register_interest(svc_id, client_id);

        let mut msg_buf = BytesMut::new();
        let (payload_bytes, payload_len) = msg.to_bytes();
        // QMI header
        msg_buf.put_u8(0x01); // magic QMI number
                              // 2 bytes total length
        msg_buf.put_u16_le(
            payload_len
                           + 3 /* flags */
                           + 2 /* length byte length */
                           // additional length is bytes not captured in the payload length
                           // They cannot be calculated there because multi-payload SDUs may
                           // exist
                           + 1 /* sdu control flag */
                           + msg.transaction_id_len() as u16,
        );

        // 1 byte control flag
        msg_buf.put_u8(0x00);
        // 1 byte svc flag
        msg_buf.put_u8(svc_id.0);
        // 1 byte client id
        msg_buf.put_u8(client_id.0);

        // SDU
        // 1 byte control flag
        msg_buf.put_u8(0x00);
        // 1 or 2 byte transaction ID
        match msg.transaction_id_len() {
            1 => msg_buf.put_u8(tx_id.0 as u8 + 1), // we know it's one byte
            2 => msg_buf.put_u16_le(tx_id.0 + 1),
            _ => panic!(
                "Unknown transaction ID length. Please add client support or fix the message \
                 definitions"
            ),
        }
        // add the payload to the buffer
        msg_buf.extend(payload_bytes);

        let bytes = msg_buf.freeze();

        if let Some(ref transport) = self.inner.transport_channel {
            if transport.is_closed() {
                fx_log_err!("Transport channel to modem is closed");
            }
            transport.write(bytes.as_ref(), &mut Vec::new()).map_err(QmuxError::ClientWrite)?
        }

        let resp = QmiResponse {
            client_id: client_id,
            svc_id: svc_id,
            tx_id: tx_id,
            transport: Some(self.inner.clone()),
        }
        .await?;

        let buf = std::io::Cursor::new(resp.bytes());
        let decoded = D::from_bytes(buf);
        Ok(decoded)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use {
        fuchsia_async::{self as fasync, DurationExt, TimeoutExt},
        fuchsia_zircon::{self as zx, DurationNum},
        futures::lock::Mutex,
        futures::{
            future::{join, join3},
            TryFutureExt,
        },
        pretty_assertions::assert_eq,
        qmi_protocol::QmiError,
        std::io,
    };

    #[should_panic]
    #[fasync::run_until_stalled(test)]
    async fn no_transport_channel() {
        // should stall without completing.
        let modem = Arc::new(Mutex::new(crate::QmiModem::new()));
        let sender = async {
            let modem_lock = modem.lock().await;
            let client = modem_lock.create_client().await;
            client
        };
        sender.await;
    }

    #[test]
    fn connect_transport_after_client_request() {
        use qmi_protocol::WDA;
        let mut executor = fasync::TestExecutor::new().unwrap();

        let (client_end, server_end) = zx::Channel::create().unwrap();
        let client_end = fasync::Channel::from_channel(client_end).unwrap();
        let modem = Arc::new(Mutex::new(crate::QmiModem::new()));
        let server = fasync::Channel::from_channel(server_end).unwrap();
        let mut buffer = zx::MessageBuf::new();

        const EXPECTED: &[u8] = &[1, 15, 0, 0, 0, 0, 0, 1, 34, 0, 4, 0, 1, 1, 0, 26];

        let sender = async {
            // hacky way of getting around two step client/lock requirements
            loop {
                if let Some(modem_lock) = modem.try_lock() {
                    let client = modem_lock.create_client().await;
                    // don't care about result, timeout
                    let _: Result<WDA::SetDataFormatResp, QmiError> = client
                        .send_msg(WDA::SetDataFormatReq::new(None, Some(0x01)))
                        .map_err(|e| {
                            io::Error::new(io::ErrorKind::Other, &*format!("fidl error: {:?}", e))
                        })
                        .on_timeout(30.millis().after_now(), || Ok(Err(QmiError::Aborted)))
                        .await
                        .unwrap();
                }
                return;
            }
        };

        let receiver = async {
            server.recv_msg(&mut buffer).await.expect("failed to recv msg");
            assert_eq!(EXPECTED, buffer.bytes());
        };

        let late_connect = async {
            let mut modem_lock = modem.lock().await;
            modem_lock.connect_transport(client_end.into());
        };

        executor.run_singlethreaded(join3(late_connect, sender, receiver));
    }

    #[test]
    fn request_id() {
        use qmi_protocol::CTL;
        const EXPECTED: &[u8] = &[1, 15, 0, 0, 0, 0, 0, 1, 34, 0, 4, 0, 1, 1, 0, 66];

        let mut executor = fasync::TestExecutor::new().unwrap();

        let (client_end, server_end) = zx::Channel::create().unwrap();
        let client_end = fasync::Channel::from_channel(client_end).unwrap();
        let mut modem = crate::QmiModem::new();
        modem.connect_transport(client_end.into());
        let modem = Arc::new(Mutex::new(modem));

        let server = fasync::Channel::from_channel(server_end).unwrap();
        let mut buffer = zx::MessageBuf::new();

        let receiver = async {
            server.recv_msg(&mut buffer).await.expect("failed to recv msg");
            assert_eq!(EXPECTED, buffer.bytes());
            let bytes =
                &[1, 15, 0, 0, 0, 0, 1, 1, 34, 0, 12, 0, 0, 4, 0, 0, 0, 0, 0, 1, 0, 0, 42, 6];
            let _ = server.write(bytes, &mut vec![]).expect("Server channel write failed");
        };

        let receiver = receiver
            .on_timeout(1000.millis().after_now(), || panic!("did not receiver message in time!"));

        let sender = async {
            let modem_lock = modem.lock().await;
            let client = modem_lock.create_client().await;
            let resp: QmiResult<CTL::GetClientIdResp> =
                client.send_msg_actual(CTL::GetClientIdReq::new(0x42)).await.unwrap();
            let msg = resp.unwrap();
            assert_eq!(msg.svc_type, 42);
            assert_eq!(msg.client_id, 6);
        };

        let sender = sender
            .on_timeout(1000.millis().after_now(), || panic!("did not receive response in time!"));

        executor.run_singlethreaded(join(receiver, sender));
    }
}
